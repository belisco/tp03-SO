/* UNIVERSIDADE FEDERAL DE MINAS GERAIS     *
 * DEPARTAMENTO DE CIENCIA DA COMPUTACAO    */

#include "pager.h"
#include "mmu.h"

#include <sys/mman.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Estrutura para representar uma página virtual */
typedef struct page {
    void *vaddr;           /* Endereço virtual da página */
    int frame;             /* Quadro de memória física (-1 se não residente) */
    int disk_block;        /* Bloco no disco (-1 se não alocado) */
    int present;           /* 1 se está na memória física, 0 caso contrário */
    int dirty;             /* 1 se foi modificada, 0 caso contrário */
    int referenced;        /* Bit de referência para segunda chance */
    int zero_filled;       /* 1 se já foi inicializada com zeros */
} page_t;

/* Estrutura para representar um processo */
typedef struct process {
    pid_t pid;
    page_t *pages;         /* Array de páginas do processo */
    int num_pages;         /* Número de páginas alocadas */
    int max_pages;         /* Capacidade do array de páginas */
} process_t;

/* Variáveis globais */
static int NFRAMES;        /* Número de quadros de memória física */
static int NBLOCKS;        /* Número de blocos no disco */
static size_t PAGESIZE;    /* Tamanho da página */

static int *frame_owner;   /* PID do dono de cada quadro (-1 se livre) */
static int *frame_page;    /* Índice da página em cada quadro */
static int *block_used;    /* 1 se o bloco está em uso, 0 caso contrário */

static process_t *processes[256]; /* Array de processos (máx 256) */
static int num_processes = 0;

static int clock_hand = 0; /* Ponteiro do algoritmo clock */

static pthread_mutex_t pager_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Funções auxiliares */
static process_t *find_process(pid_t pid);
static int alloc_frame(void);
static int alloc_disk_block(void);
static void free_frame(int frame);
static void free_disk_block(int block);
static int evict_page(void);
//static void handle_page_access(pid_t pid, page_t *page, int write_access);

void pager_init(int nframes, int nblocks) {
    NFRAMES = nframes;
    NBLOCKS = nblocks;
    PAGESIZE = sysconf(_SC_PAGESIZE);
    
    /* Inicializa arrays de controle de quadros */
    frame_owner = malloc(NFRAMES * sizeof(int));
    frame_page = malloc(NFRAMES * sizeof(int));
    for (int i = 0; i < NFRAMES; i++) {
        frame_owner[i] = -1;
        frame_page[i] = -1;
    }
    
    /* Inicializa array de controle de blocos de disco */
    block_used = malloc(NBLOCKS * sizeof(int));
    for (int i = 0; i < NBLOCKS; i++) {
        block_used[i] = 0;
    }
    
    /* Inicializa array de processos */
    for (int i = 0; i < 256; i++) {
        processes[i] = NULL;
    }
}

void pager_create(pid_t pid) {
    pthread_mutex_lock(&pager_mutex);
    
    process_t *proc = malloc(sizeof(process_t));
    proc->pid = pid;
    proc->num_pages = 0;
    proc->max_pages = 16; /* Capacidade inicial */
    proc->pages = malloc(proc->max_pages * sizeof(page_t));
    
    /* Adiciona processo ao array */
    processes[num_processes++] = proc;
    
    pthread_mutex_unlock(&pager_mutex);
}

void *pager_extend(pid_t pid) {
    pthread_mutex_lock(&pager_mutex);
    
    process_t *proc = find_process(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager_mutex);
        return NULL;
    }
    
    /* Verifica se há blocos de disco disponíveis */
    int disk_block = alloc_disk_block();
    if (disk_block == -1) {
        pthread_mutex_unlock(&pager_mutex);
        return NULL;
    }
    
    /* Expande array de páginas se necessário */
    if (proc->num_pages >= proc->max_pages) {
        proc->max_pages *= 2;
        proc->pages = realloc(proc->pages, proc->max_pages * sizeof(page_t));
    }
    
    /* Calcula endereço virtual da nova página */
    void *vaddr = (void *)(UVM_BASEADDR + proc->num_pages * PAGESIZE);
    
    /* Inicializa nova página */
    page_t *page = &proc->pages[proc->num_pages];
    page->vaddr = vaddr;
    page->frame = -1;
    page->disk_block = disk_block;
    page->present = 0;
    page->dirty = 0;
    page->referenced = 0;
    page->zero_filled = 0;
    
    proc->num_pages++;
    
    pthread_mutex_unlock(&pager_mutex);
    return vaddr;
}

void pager_fault(pid_t pid, void *addr) {
    pthread_mutex_lock(&pager_mutex);
    
    process_t *proc = find_process(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager_mutex);
        return;
    }
    
    /* Encontra a página correspondente ao endereço */
    intptr_t offset = (intptr_t)addr - UVM_BASEADDR;
    int page_idx = offset / PAGESIZE;
    
    if (page_idx < 0 || page_idx >= proc->num_pages) {
        pthread_mutex_unlock(&pager_mutex);
        return;
    }
    
    page_t *page = &proc->pages[page_idx];
    
    /* Verifica se a página já está presente */
    if (page->present && page->frame != -1) {
        /* Página presente, apenas atualiza permissões e marca como referenciada */
        page->referenced = 1;
        page->dirty = 1; /* Assume escrita, pois não sabemos o tipo de acesso */
        mmu_chprot(pid, page->vaddr, PROT_READ | PROT_WRITE);
        pthread_mutex_unlock(&pager_mutex);
        return;
    }
    
    /* Aloca um quadro de memória */
    int frame = alloc_frame();
    if (frame == -1) {
        /* Não há quadros livres, precisa fazer eviction */
        frame = evict_page();
    }
    
    /* Atribui o quadro ao processo */
    frame_owner[frame] = pid;
    frame_page[frame] = page_idx;
    page->frame = frame;
    
    /* Se a página nunca foi inicializada, preenche com zeros */
    if (!page->zero_filled) {
        mmu_zero_fill(frame);
        page->zero_filled = 1;
        page->dirty = 0; /* Página limpa após zero_fill */
    } else {
        /* Carrega página do disco */
        mmu_disk_read(page->disk_block, frame);
        page->dirty = 0; /* Página limpa após carregar do disco */
    }
    
    /* Mapeia a página na memória com permissões iniciais apenas de leitura */
    /* Isso permite detectar escritas na próxima falha */
    page->present = 1;
    page->referenced = 0; /* Começa sem referência, será marcada no próximo acesso */
    mmu_resident(pid, page->vaddr, frame, PROT_READ);
    
    pthread_mutex_unlock(&pager_mutex);
}

int pager_syslog(pid_t pid, void *addr, size_t len) {
    pthread_mutex_lock(&pager_mutex);
    
    process_t *proc = find_process(pid);
    if (!proc) {
        pthread_mutex_unlock(&pager_mutex);
        errno = EINVAL;
        return -1;
    }
    
    /* Verifica se o endereço inicial está no espaço alocado */
    intptr_t start = (intptr_t)addr;
    intptr_t end = start + len - 1;
    
    if (start < UVM_BASEADDR || end >= UVM_BASEADDR + proc->num_pages * PAGESIZE) {
        pthread_mutex_unlock(&pager_mutex);
        errno = EINVAL;
        return -1;
    }
    
    /* Buffer temporário para armazenar dados */
    char *buffer = malloc(len);
    
    /* Lê os dados byte a byte */
    for (size_t i = 0; i < len; i++) {
        intptr_t curr_addr = start + i;
        int page_idx = (curr_addr - UVM_BASEADDR) / PAGESIZE;
        int page_offset = (curr_addr - UVM_BASEADDR) % PAGESIZE;
        
        page_t *page = &proc->pages[page_idx];
        
        /* Se a página não está presente, precisa carregá-la */
        if (!page->present || page->frame == -1) {
            /* Libera mutex temporariamente para evitar deadlock */
            pthread_mutex_unlock(&pager_mutex);
            pager_fault(pid, (void *)(UVM_BASEADDR + page_idx * PAGESIZE));
            pthread_mutex_lock(&pager_mutex);
            
            /* Re-valida o processo após reacquirir o lock */
            proc = find_process(pid);
            if (!proc) {
                free(buffer);
                pthread_mutex_unlock(&pager_mutex);
                errno = EINVAL;
                return -1;
            }
            page = &proc->pages[page_idx];
        }
        
        /* Garante que temos um frame válido */
        if (page->frame == -1 || !page->present) {
            free(buffer);
            pthread_mutex_unlock(&pager_mutex);
            errno = EINVAL;
            return -1;
        }
        
        /* Lê o byte da memória física */
        buffer[i] = pmem[page->frame * PAGESIZE + page_offset];
    }
    
    /* Imprime em formato hexadecimal */
    for (size_t i = 0; i < len; i++) {
        printf("%02x", (unsigned char)buffer[i]);
    }
    
    free(buffer);
    pthread_mutex_unlock(&pager_mutex);
    return 0;
}

void pager_destroy(pid_t pid) {
    pthread_mutex_lock(&pager_mutex);
    
    process_t *proc = NULL;
    int proc_idx = -1;
    
    /* Encontra o processo */
    for (int i = 0; i < num_processes; i++) {
        if (processes[i] && processes[i]->pid == pid) {
            proc = processes[i];
            proc_idx = i;
            break;
        }
    }
    
    if (!proc) {
        pthread_mutex_unlock(&pager_mutex);
        return;
    }
    
    /* Libera todos os recursos do processo */
    for (int i = 0; i < proc->num_pages; i++) {
        page_t *page = &proc->pages[i];
        
        /* Libera quadro de memória se alocado */
        if (page->present && page->frame != -1) {
            free_frame(page->frame);
        }
        
        /* Libera bloco de disco */
        if (page->disk_block != -1) {
            free_disk_block(page->disk_block);
        }
    }
    
    /* Libera estruturas do processo */
    free(proc->pages);
    free(proc);
    processes[proc_idx] = NULL;
    
    pthread_mutex_unlock(&pager_mutex);
}

/* Funções auxiliares */

static process_t *find_process(pid_t pid) {
    for (int i = 0; i < num_processes; i++) {
        if (processes[i] && processes[i]->pid == pid) {
            return processes[i];
        }
    }
    return NULL;
}

static int alloc_frame(void) {
    for (int i = 0; i < NFRAMES; i++) {
        if (frame_owner[i] == -1) {
            return i;
        }
    }
    return -1;
}

static int alloc_disk_block(void) {
    for (int i = 0; i < NBLOCKS; i++) {
        if (!block_used[i]) {
            block_used[i] = 1;
            return i;
        }
    }
    return -1;
}

static void free_frame(int frame) {
    frame_owner[frame] = -1;
    frame_page[frame] = -1;
}

static void free_disk_block(int block) {
    block_used[block] = 0;
}

static int evict_page(void) {
    /* Algoritmo da segunda chance (clock) */
    int iterations = 0;
    int max_iterations = NFRAMES * 2; /* Evita loop infinito */
    
    while (iterations < max_iterations) {
        int frame = clock_hand;
        clock_hand = (clock_hand + 1) % NFRAMES;
        iterations++;
        
        if (frame_owner[frame] == -1) {
            /* Quadro livre, usa ele */
            return frame;
        }
        
        pid_t owner_pid = frame_owner[frame];
        int page_idx = frame_page[frame];
        
        process_t *proc = find_process(owner_pid);
        if (!proc || page_idx >= proc->num_pages) {
            /* Processo não existe mais ou página inválida, libera o quadro */
            free_frame(frame);
            return frame;
        }
        
        page_t *page = &proc->pages[page_idx];
        
        /* Segunda chance: verifica bit de referência */
        if (page->referenced) {
            /* Dá segunda chance: limpa bit e remove permissões */
            page->referenced = 0;
            mmu_chprot(owner_pid, page->vaddr, PROT_NONE);
            continue;
        }
        
        /* Página escolhida para eviction */
        
        /* Se a página foi modificada, salva no disco */
        if (page->dirty) {
            mmu_disk_write(frame, page->disk_block);
        }
        
        /* Remove mapeamento */
        mmu_nonresident(owner_pid, page->vaddr);
        page->present = 0;
        page->frame = -1;
        page->dirty = 0;
        page->referenced = 0;
        
        /* Libera o quadro */
        free_frame(frame);
        
        return frame;
    }
    
    /* Se chegou aqui, força eviction do frame atual */
    int frame = clock_hand;
    clock_hand = (clock_hand + 1) % NFRAMES;
    
    if (frame_owner[frame] != -1) {
        pid_t owner_pid = frame_owner[frame];
        int page_idx = frame_page[frame];
        process_t *proc = find_process(owner_pid);
        
        if (proc && page_idx < proc->num_pages) {
            page_t *page = &proc->pages[page_idx];
            if (page->dirty) {
                mmu_disk_write(frame, page->disk_block);
            }
            mmu_nonresident(owner_pid, page->vaddr);
            page->present = 0;
            page->frame = -1;
            page->dirty = 0;
            page->referenced = 0;
        }
    }
    
    free_frame(frame);
    return frame;
}