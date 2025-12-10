/* Test 13: syslog spanning multiple pages
 * Testa se pager_syslog consegue ler dados que atravessam múltiplas páginas.
 * Isso força o paginador a lidar com chunks de dados em páginas diferentes.
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
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    
    uvm_create();
    
    char *page0 = uvm_extend();
    char *page1 = uvm_extend();
    char *page2 = uvm_extend();
    
    assert(page0 != NULL);
    assert(page1 != NULL);
    assert(page2 != NULL);
    
    //Escreve dados no final da página 0
    strcpy(page0 + PAGESIZE - 10, "ABCDEFGHI");
    
    //Escreve dados no início da página 1
    strcpy(page1, "JKLMNOPQR");
    
    //Escreve dados no meio da página 1
    strcpy(page1 + PAGESIZE/2, "STUVWXYZ");
    
    //syslog atravessando página 0 e 1 (20 bytes)
    uvm_syslog(page0 + PAGESIZE - 10, 20);
    
    //syslog atravessando do meio da página 1 até página 
    strcpy(page2, "0123456789");
    uvm_syslog(page1 + PAGESIZE/2, PAGESIZE/2 + 10);
    
    //syslog atravessando as 3 páginas
    uvm_syslog(page0 + PAGESIZE - 5, PAGESIZE + 10);
    
    exit(EXIT_SUCCESS);
}
