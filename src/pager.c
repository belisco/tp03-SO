#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mmu.h"
#include "pager.h"

#define MAX_PROCS 128
#define MAX_PAGES 256   /* 1MiB / 4KiB = 256 páginas */

/* Informação de cada frame físico */
typedef struct {
    int used;           /* frame está em uso */
    pid_t pid;          /* dono do frame */
    int page;           /* índice da página virtual do processo */
    int ref;            /* bit de referência (segunda chance) */
    int prot;           /* PROT_NONE, PROT_READ ou PROT_READ|PROT_WRITE */
} FrameInfo;

/* Informação de cada bloco de disco */
typedef struct {
    int used;
    pid_t pid;
    int page;           /* página virtual correspondente */
} BlockInfo;

/* Informação de cada página virtual de um processo */
typedef struct {
    int allocated;      /* página foi alocada via pager_extend */
    int resident;       /* está em algum frame físico? */
    int frame;          /* índice do frame, se resident */
    int disk_block;     /* bloco de disco reservado */
    int in_disk;        /* conteúdo válido salvo em disco? */
    int dirty;          /* página foi modificada desde o último write em disco? */
} PageInfo;

/* Informação de cada processo conhecido pelo pager */
typedef struct {
    int used;
    pid_t pid;
    int npages;                 /* número de páginas alocadas */
    PageInfo pages[MAX_PAGES];
} ProcInfo;

/* ------------------------------------------------------------------ */
/* Estado global do pager                                             */
/* ------------------------------------------------------------------ */

static FrameInfo *frames = NULL;
static BlockInfo *blocks = NULL;
static int g_nframes = 0;
static int g_nblocks = 0;
static long g_pagesize = 0;
static int clock_hand = 0;          /* ponteiro do algoritmo clock */

static ProcInfo procs[MAX_PROCS];

static pthread_mutex_t pager_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Funções auxiliares                                                 */
/* ------------------------------------------------------------------ */

static ProcInfo *find_proc(pid_t pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && procs[i].pid == pid)
            return &procs[i];
    }
    return NULL;
}

static ProcInfo *create_proc_entry(pid_t pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].used) {
            procs[i].used = 1;
            procs[i].pid = pid;
            procs[i].npages = 0;
            memset(procs[i].pages, 0, sizeof(procs[i].pages));
            return &procs[i];
        }
    }
    return NULL;
}

static int alloc_block(pid_t pid, int page_index) {
    for (int i = 0; i < g_nblocks; i++) {
        if (!blocks[i].used) {
            blocks[i].used = 1;
            blocks[i].pid = pid;
            blocks[i].page = page_index;
            return i;
        }
    }
    return -1;
}

static void free_block(int blk) {
    if (blk < 0 || blk >= g_nblocks) return;
    blocks[blk].used = 0;
    blocks[blk].pid = 0;
    blocks[blk].page = 0;
}

static int first_free_frame(void) {
    for (int i = 0; i < g_nframes; i++) {
        if (!frames[i].used)
            return i;
    }
    return -1;
}

/* Escolhe vítima pelo algoritmo de segunda chance (clock). */
static int choose_victim_frame(void) {
    while (1) {
        FrameInfo *f = &frames[clock_hand];

        /* Se por acaso encontrar um frame livre, usa ele mesmo. */
        if (!f->used) {
            int idx = clock_hand;
            clock_hand = (clock_hand + 1) % g_nframes;
            return idx;
        }

        /* Se ref == 0, este frame pode ser vítima. */
        if (f->ref == 0) {
            int idx = clock_hand;
            clock_hand = (clock_hand + 1) % g_nframes;
            return idx;
        }

        /* Segunda chance: zera ref e tira permissão (PROT_NONE). */
        ProcInfo *p = find_proc(f->pid);
        if (p) {
            void *vaddr = (void *)(UVM_BASEADDR +
                                   (intptr_t)f->page * g_pagesize);
            mmu_chprot(f->pid, vaddr, PROT_NONE);
            f->prot = PROT_NONE;
            f->ref = 0;
        }

        clock_hand = (clock_hand + 1) % g_nframes;
    }
}

/* Evicta a página atualmente no frame `frame` para o disco. */
static void evict_frame(int frame) {
    FrameInfo *f = &frames[frame];
    if (!f->used)
        return;

    ProcInfo *p = find_proc(f->pid);
    if (!p)
        return;

    if (f->page < 0 || f->page >= MAX_PAGES)
        return;

    PageInfo *pg = &p->pages[f->page];
    if (!pg->allocated || !pg->resident)
        return;

    void *vaddr = (void *)(UVM_BASEADDR +
                           (intptr_t)f->page * g_pagesize);

    /* O professor espera: primeiro NONRESIDENT, depois DISK_WRITE. */
    mmu_nonresident(p->pid, vaddr);

    /* Escreve em disco apenas se a página estiver suja. */
    if (pg->dirty && pg->disk_block >= 0) {
        mmu_disk_write(frame, pg->disk_block);
        pg->in_disk = 1;
        pg->dirty = 0;      /* disco agora tem a cópia atual */
    }

    pg->resident = 0;
    pg->frame = -1;

    f->used = 0;
    f->pid = 0;
    f->page = -1;
    f->ref = 0;
    f->prot = PROT_NONE;
}


/* Garante que a página `page_index` do processo `p` esteja residente.
 * Retorna o índice do frame físico que contém a página.  Esta função
 * é usada tanto pelo pager_fault (para páginas não residentes) quanto
 * pelo pager_syslog.  Ela se comporta como um acesso de LEITURA. */
static int ensure_page_resident(ProcInfo *p, int page_index) {
    PageInfo *pg = &p->pages[page_index];

    /* Já residente: apenas marca como referenciada e, se estiver com
     * PROT_NONE (segunda chance), restaura o prot adequado. */
    if (pg->resident) {
        int frame = pg->frame;
        FrameInfo *f = &frames[frame];

        if (f->prot == PROT_NONE) {
            void *vaddr = (void *)(UVM_BASEADDR +
                                   (intptr_t)page_index * g_pagesize);
            int newprot = pg->dirty ? (PROT_READ | PROT_WRITE) : PROT_READ;
            mmu_chprot(p->pid, vaddr, newprot);
            f->prot = newprot;
        }

        f->ref = 1;
        return frame;
    }

    /* Precisa de frame novo. */
    int frame = first_free_frame();
    if (frame < 0) {
        frame = choose_victim_frame();
        evict_frame(frame);
    }

    /* Carrega conteúdo: se já existe em disco -> disk_read;
     * caso contrário, página nova -> zero_fill. */
    if (pg->in_disk && pg->disk_block >= 0) {
        mmu_disk_read(pg->disk_block, frame);
    } else {
        mmu_zero_fill(frame);
    }

    void *vaddr = (void *)(UVM_BASEADDR +
                           (intptr_t)page_index * g_pagesize);

    /* Ao trazer a página para RAM pela primeira vez (ou após swap),
     * começamos sempre com PROT_READ.  Escrever nela causará um novo
     * page fault, onde marcaremos como suja e habilitaremos WRITE. */
    mmu_resident(p->pid, vaddr, frame, PROT_READ);

    FrameInfo *f = &frames[frame];
    f->used = 1;
    f->pid = p->pid;
    f->page = page_index;
    f->ref = 1;
    f->prot = PROT_READ;

    pg->resident = 1;
    pg->frame = frame;
    /* ao carregar de disco, o conteúdo está sincronizado → dirty = 0 */
    pg->dirty = 0;

    return frame;
}

/* ------------------------------------------------------------------ */
/* Implementação das funções do pager                                 */
/* ------------------------------------------------------------------ */

void pager_init(int nframes, int nblocks) {
    pthread_mutex_lock(&pager_lock);

    g_nframes = nframes;
    g_nblocks = nblocks;
    g_pagesize = sysconf(_SC_PAGESIZE);

    frames = calloc(g_nframes, sizeof(FrameInfo));
    blocks = calloc(g_nblocks, sizeof(BlockInfo));
    memset(procs, 0, sizeof(procs));
    clock_hand = 0;

    pthread_mutex_unlock(&pager_lock);
}

void pager_create(pid_t pid) {
    pthread_mutex_lock(&pager_lock);

    ProcInfo *p = find_proc(pid);
    if (!p) {
        create_proc_entry(pid);
    }

    pthread_mutex_unlock(&pager_lock);
}

void *pager_extend(pid_t pid) {
    pthread_mutex_lock(&pager_lock);

    ProcInfo *p = find_proc(pid);
    if (!p) {
        p = create_proc_entry(pid);
        if (!p) {
            pthread_mutex_unlock(&pager_lock);
            errno = ENOMEM;
            return NULL;
        }
    }

    if (p->npages >= MAX_PAGES) {
        pthread_mutex_unlock(&pager_lock);
        errno = ENOMEM;
        return NULL;
    }

    int page_index = p->npages;

    int blk = alloc_block(pid, page_index);
    if (blk < 0) {
        pthread_mutex_unlock(&pager_lock);
        errno = ENOSPC;
        return NULL;
    }

    PageInfo *pg = &p->pages[page_index];
    pg->allocated = 1;
    pg->resident = 0;
    pg->frame = -1;
    pg->disk_block = blk;
    pg->in_disk = 0;
    pg->dirty = 0;

    p->npages++;

    void *vaddr = (void *)(UVM_BASEADDR +
                           (intptr_t)page_index * g_pagesize);

    pthread_mutex_unlock(&pager_lock);
    return vaddr;
}

void pager_fault(pid_t pid, void *addr) {
    pthread_mutex_lock(&pager_lock);

    ProcInfo *p = find_proc(pid);
    if (!p) {
        pthread_mutex_unlock(&pager_lock);
        return;
    }

    intptr_t a = (intptr_t)addr;
    intptr_t offset = a - (intptr_t)UVM_BASEADDR;
    if (offset < 0) {
        pthread_mutex_unlock(&pager_lock);
        return;
    }

    int page_index = (int)(offset / g_pagesize);
    if (page_index < 0 || page_index >= p->npages) {
        pthread_mutex_unlock(&pager_lock);
        return;
    }

    PageInfo *pg = &p->pages[page_index];
    if (!pg->allocated) {
        pthread_mutex_unlock(&pager_lock);
        return;
    }

    if (!pg->resident) {
        /* Página ainda não residente: traz para RAM com PROT_READ. */
        ensure_page_resident(p, page_index);
        pthread_mutex_unlock(&pager_lock);
        return;
    }

    /* Página já está residente: o fault veio de PROT_NONE (segunda
     * chance) ou PROT_READ (primeira escrita). */
    int frame = pg->frame;
    FrameInfo *f = &frames[frame];

    void *vaddr = (void *)(UVM_BASEADDR +
                           (intptr_t)page_index * g_pagesize);

    if (f->prot == PROT_NONE) {
        /* Página teve segunda chance e foi tocada de novo:
         * restaura prot conforme dirty. */
        int newprot = pg->dirty ? (PROT_READ | PROT_WRITE) : PROT_READ;
        mmu_chprot(pid, vaddr, newprot);
        f->prot = newprot;
        f->ref = 1;
    } else if (f->prot == PROT_READ) {
        /* Primeira escrita na página: marca como suja e habilita WRITE. */
        mmu_chprot(pid, vaddr, PROT_READ | PROT_WRITE);
        f->prot = PROT_READ | PROT_WRITE;
        f->ref = 1;
        pg->dirty = 1;
    } else {
        /* PROT_READ|PROT_WRITE: em teoria não deveríamos chegar aqui. */
        f->ref = 1;
    }

    pthread_mutex_unlock(&pager_lock);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    pthread_mutex_lock(&pager_lock);

    ProcInfo *p = find_proc(pid);
    if (!p) {
        pthread_mutex_unlock(&pager_lock);
        errno = EINVAL;
        return -1;
    }

    if (len == 0) {
        pthread_mutex_unlock(&pager_lock);
        return 0;
    }

    intptr_t start = (intptr_t)addr;
    intptr_t base  = (intptr_t)UVM_BASEADDR;
    intptr_t limit = base + (intptr_t)p->npages * g_pagesize;

    /* Verifica se [addr, addr+len) está dentro das páginas alocadas. */
    if (start < base || start + (intptr_t)len > limit) {
        pthread_mutex_unlock(&pager_lock);
        errno = EINVAL;
        return -1;
    }

    char *buf = malloc(len);
    if (!buf) {
        int err = errno;
        pthread_mutex_unlock(&pager_lock);
        errno = err;
        return -1;
    }

    size_t pos = 0;
    while (pos < len) {
        intptr_t cur = start + (intptr_t)pos;
        int page_index = (int)((cur - base) / g_pagesize);
        int offset     = (int)((cur - base) % g_pagesize);

        if (page_index < 0 || page_index >= p->npages) {
            free(buf);
            pthread_mutex_unlock(&pager_lock);
            errno = EINVAL;
            return -1;
        }

        int frame = ensure_page_resident(p, page_index);

        size_t chunk = (size_t)(g_pagesize - offset);
        if (chunk > len - pos)
            chunk = len - pos;

        const char *frame_base = pmem + (size_t)frame * g_pagesize;
        memcpy(buf + pos, frame_base + offset, chunk);

        pos += chunk;
    }

    pthread_mutex_unlock(&pager_lock);

    /* Imprime os bytes em hexadecimal, como especificado. */
    for (size_t i = 0; i < len; i++) {
        printf("%02x", (unsigned char)buf[i]);
    }

    printf("\n");

    free(buf);
    return 0;
}

void pager_destroy(pid_t pid) {
    pthread_mutex_lock(&pager_lock);

    ProcInfo *p = find_proc(pid);
    if (!p) {
        pthread_mutex_unlock(&pager_lock);
        return;
    }

    for (int i = 0; i < p->npages; i++) {
        PageInfo *pg = &p->pages[i];
        if (!pg->allocated)
            continue;

        /* Libera frame na nossa estrutura (NÃO chama mmu_* aqui). */
        if (pg->resident && pg->frame >= 0 && pg->frame < g_nframes) {
            FrameInfo *f = &frames[pg->frame];
            f->used = 0;
            f->pid  = 0;
            f->page = -1;
            f->ref  = 0;
            f->prot = PROT_NONE;
        }

        if (pg->disk_block >= 0)
            free_block(pg->disk_block);

        pg->allocated = 0;
        pg->resident  = 0;
        pg->frame     = -1;
        pg->disk_block = -1;
        pg->in_disk   = 0;
        pg->dirty     = 0;
    }

    p->used = 0;
    p->npages = 0;

    pthread_mutex_unlock(&pager_lock);
}
