/* Test 19: Sequential access pattern
 * Testa um padrão de acesso sequencial típico: aloca páginas,
 * escreve sequencialmente, lê sequencialmente.
 */
#include <sys/types.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "uvm.h"

int main(void) {
    uvm_create();
    
    //Aloca 6 páginas
    char *pages[6];
    for (int i = 0; i < 6; i++) {
        pages[i] = uvm_extend();
        assert(pages[i] != NULL);
    }
    
    //Fase 1: Escrita sequencial
    for (int i = 0; i < 6; i++) {
        //Preenche a página com um padrão
        memset(pages[i], 'A' + i, 100);
        pages[i][100] = '\0';
    }
    
    //Fase 2: Leitura sequencial
    for (int i = 0; i < 6; i++) {
        //Verifica o padrão
        for (int j = 0; j < 100; j++) {
            assert(pages[i][j] == 'A' + i);
        }
        assert(pages[i][100] == '\0');
    }
    
    //Fase 3: Modificação parcial
    for (int i = 0; i < 6; i++) {
        pages[i][50] = 'X';
    }
    
    //Fase 4: Verificação após swap
    for (int i = 0; i < 6; i++) {
        //Acessa o início (deve estar preservado)
        assert(pages[i][0] == 'A' + i);
        //Acessa o meio (modificado)
        assert(pages[i][50] == 'X');
        //Acessa próximo ao fim
        assert(pages[i][99] == 'A' + i);
    }
    
    //Fase 5: syslog de cada página
    for (int i = 0; i < 6; i++) {
        uvm_syslog(pages[i], 101);
    }
    
    printf("Sequential access pattern completed\n");
    exit(EXIT_SUCCESS);
}
