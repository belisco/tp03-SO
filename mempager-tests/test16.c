/* Test 16: Read-only vs read-write pages
 * Testa o comportamento de páginas que são apenas lidas vs páginas escritas.
 * Verifica se o algoritmo de segunda chance distingue dirty pages corretamente.
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
    
    //Aloca 5 páginas
    char *pages[5];
    for (int i = 0; i < 5; i++) {
        pages[i] = uvm_extend();
        assert(pages[i] != NULL);
    }
    
	//Apenas lê as 3 primeiras páginas (não-dirty)
    for (int i = 0; i < 3; i++) {
        char c = pages[i][0]; //Força page fault de leitura
        (void)c; //Evita warning de variável não usada
    }
    
    //Escreve nas 2 últimas (dirty)
    pages[3][0] = 'X';
    pages[4][0] = 'Y';
    
    //Lê novamente as 3 primeiras
    for (int i = 0; i < 3; i++) {
        char c = pages[i][100];
        (void)c;
    }
    
    //Agora força swap: aloca mais páginas
    for (int i = 0; i < 3; i++) {
        char *extra = uvm_extend();
        assert(extra != NULL);
        extra[0] = 'Z' + i; //Força escrita
    }
    
    //Acessa todas as páginas originais novamente
    for (int i = 0; i < 5; i++) {
        char c = pages[i][0];
        (void)c;
    }
    
    //Verifica que as escritas persistiram
    assert(pages[3][0] == 'X');
    assert(pages[4][0] == 'Y');
    
    printf("Read-only and read-write pages handled correctly\n");
    
    exit(EXIT_SUCCESS);
}
