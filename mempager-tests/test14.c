/*Test 14: Thrashing test
 * Aloca mais páginas do que frames disponíveis e acessa alternadamente.
 * Força o algoritmo de segunda chance a trabalhar intensamente.
 */
#include <sys/types.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "uvm.h"

int main(void) {
    uvm_create();
    
    //Aloca 6 páginas (mais que os 4 frames disponíveis)
    char *pages[6];
    for (int i = 0; i < 6; i++) {
        pages[i] = uvm_extend();
        assert(pages[i] != NULL);
    }
    
    //Escreve em todas as páginas
    for (int i = 0; i < 6; i++) {
        pages[i][0] = 'A' + i;
        pages[i][1] = '\0';
    }
    
    //Acessa as páginas em ordem alternada para causar thrashing
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 6; i++) {
            //Lê e verifica o conteúdo
            char expected = 'A' + i;
            if (pages[i][0] != expected) {
                printf("ERROR: page %d expected '%c' got '%c'\n", 
                       i, expected, pages[i][0]);
                exit(EXIT_FAILURE);
            }
            //Modifica para forçar dirty bit
            pages[i][0] = expected;
        }
    }
    
    //Acessa em ordem reversa 
    for (int i = 5; i >= 0; i--) {
        uvm_syslog(pages[i], 2);
    }
    
    exit(EXIT_SUCCESS);
}
