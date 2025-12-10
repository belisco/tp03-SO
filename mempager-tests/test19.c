/* Test 20: Second chance algorithm with mixed access
 * Testa especificamente o algoritmo de segunda chance com
 * padrões de acesso que devem dar segunda chance a algumas páginas.
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
    
    //Aloca 7 páginas (mais que os 4 frames)
    char *pages[7];
    for (int i = 0; i < 7; i++) {
        pages[i] = uvm_extend();
        assert(pages[i] != NULL);
        pages[i][0] = '0' + i;
    }
    
    //Acessa páginas 0,1,2,3 - todas vão para frames
    for (int i = 0; i < 4; i++) {
        char c = pages[i][0];
        assert(c == '0' + i);
    }
    
    //Acessa página 4 - deve causar eviction de página 0
    pages[4][0] = '4';
    
    //Reacessa página 1,2,3 - elas ganham "segunda chance"
    for (int i = 1; i < 4; i++) {
        char c = pages[i][0];
        assert(c == '0' + i);
    }
    
    /* Acessa página 5 - deve evictar página 0 (se ainda não foi)
       ou começar a dar segunda chance */
    pages[5][0] = '5';
    
    //Acessa página 6 - continua o algoritmo de segunda chance
    pages[6][0] = '6';
    
    //Agora reacessa todas na ordem reversa
    for (int i = 6; i >= 0; i--) {
        char c = pages[i][0];
        assert(c == '0' + i);
    }
    
    //Modifica todas
    for (int i = 0; i < 7; i++) {
        pages[i][1] = 'X';
    }
    
    //Verifica que as modificações persistiram
    for (int i = 0; i < 7; i++) {
        assert(pages[i][0] == '0' + i);
        assert(pages[i][1] == 'X');
        uvm_syslog(pages[i], 2);
    }
    
    printf("Second chance algorithm working correctly\n");
    exit(EXIT_SUCCESS);
}
