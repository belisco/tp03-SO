/* Test 18: Disk block exhaustion
 * Testa o comportamento quando os blocos de disco se esgotam.
 * pager_extend deve retornar NULL e errno deve ser ENOSPC.
 */
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "uvm.h"

int main(void) {
    uvm_create();
    
    //Com 8 blocos de disco disponíveis, tentamos alocar 10 páginas
    char *pages[10];
    int allocated = 0;
    
    for (int i = 0; i < 10; i++) {
        pages[i] = uvm_extend();
        
        if (pages[i] == NULL) {
            //Deve falhar quando os blocos acabarem
            if (errno != ENOSPC) {
                printf("ERROR: expected errno=ENOSPC, got errno=%d\n", errno);
                exit(EXIT_FAILURE);
            }
            printf("Disk exhausted after %d pages (expected)\n", i);
            break;
        }
        
        allocated++;
        
        //Escreve algo na página para marcar como alocada
        pages[i][0] = 'A' + i;
    }
    
    //Deve ter alocado pelo menos 1 página e no máximo 8
    if (allocated < 1 || allocated > 8) {
        printf("ERROR: allocated %d pages, expected 1-8\n", allocated);
        exit(EXIT_FAILURE);
    }
    
    printf("Successfully allocated %d pages\n", allocated);
    
    //Tenta alocar mais uma vez (deve falhar)
    char *extra = uvm_extend();
    if (extra != NULL) {
        printf("ERROR: should not be able to allocate after exhaustion\n");
        exit(EXIT_FAILURE);
    }
    if (errno != ENOSPC) {
        printf("ERROR: expected errno=ENOSPC after exhaustion, got errno=%d\n", errno);
        exit(EXIT_FAILURE);
    }
    
    //Verifica que as páginas alocadas ainda são acessíveis
    for (int i = 0; i < allocated; i++) {
        if (pages[i][0] != 'A' + i) {
            printf("ERROR: page %d corrupted\n", i);
            exit(EXIT_FAILURE);
        }
        uvm_syslog(pages[i], 1);
    }
    
    printf("Disk exhaustion handled correctly\n");
    exit(EXIT_SUCCESS);
}
