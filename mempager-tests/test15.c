/* Test 15: Edge cases for syslog
 * Testa casos extremos: len=0, endereços fora dos limites, NULL, etc.
 */
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "mmu.h"
#include "uvm.h"

int main(void) {
    uvm_create();
    
    char *page0 = uvm_extend();
    assert(page0 != NULL);
    
    strcpy(page0, "test");
    
    //Caso 1: len = 0 (deve retornar 0)
    int r = uvm_syslog(page0, 0);
    assert(r == 0);
    
    //Caso 2: endereço NULL (deve falhar)
    r = uvm_syslog(NULL, 10);
    assert(r == -1);
    assert(errno == EINVAL);
    
    //Caso 3: endereço antes de UVM_BASEADDR (deve falhar)
    r = uvm_syslog((char *)UVM_BASEADDR - 100, 10);
    assert(r == -1);
    assert(errno == EINVAL);
    
    //Caso 4: endereço além do alocado (deve falhar)
    size_t PAGESIZE = sysconf(_SC_PAGESIZE);
    r = uvm_syslog(page0 + PAGESIZE, 10);
    assert(r == -1);
    assert(errno == EINVAL);
    
    //Caso 5: syslog que ultrapassa o limite alocado (deve falhar)
    r = uvm_syslog(page0 + PAGESIZE - 5, 10);
    assert(r == -1);
    assert(errno == EINVAL);
    
    //Caso 6: syslog válido no limite exato
    r = uvm_syslog(page0 + PAGESIZE - 5, 5);
    assert(r == 0);
    
    //Caso 7: syslog de página inteira
    r = uvm_syslog(page0, PAGESIZE);
    assert(r == 0);
    
    exit(EXIT_SUCCESS);
}
