/* Test 17: Multiple processes competing for resources
 * Testa múltiplos processos alocando e acessando memória simultaneamente.
 * Verifica isolamento entre processos e gerenciamento correto de recursos.
 */
#include <sys/types.h>
#include <sys/wait.h>
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
    int num_children = 2;
    
    for (int i = 0; i < num_children; i++) {
        pid_t pid = fork();
        
        if (pid == 0) {
            //Processo filho
            pid_t my_pid = getpid();
            uvm_create();
            
            //Cada processo aloca 3 páginas
            char *pages[3];
            for (int j = 0; j < 3; j++) {
                pages[j] = uvm_extend();
                assert(pages[j] != NULL);
                
                //Escreve o PID em cada página
                sprintf(pages[j], "PID:%d-PAGE:%d", (int)my_pid, j);
            }
            
            //Acessa as páginas várias vezes
            for (int round = 0; round < 10; round++) {
                for (int j = 0; j < 3; j++) {
                    char expected[64];
                    sprintf(expected, "PID:%d-PAGE:%d", (int)my_pid, j);
                    
                    //Verifica integridade
                    if (strncmp(pages[j], expected, strlen(expected)) != 0) {
                        printf("ERROR: Process %d page %d corrupted!\n",
                               (int)my_pid, j);
                        exit(EXIT_FAILURE);
                    }
                    
                    //Reescreve para forçar dirty
                    sprintf(pages[j], "PID:%d-PAGE:%d", (int)my_pid, j);
                }
                
                //Faz syslog de uma página aleatória
                int page_idx = round % 3;
                uvm_syslog(pages[page_idx], strlen(pages[page_idx]) + 1);
            }
            
            exit(EXIT_SUCCESS);
        } else if (pid < 0) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
    }
    
    //Processo pai espera todos os filhos
    for (int i = 0; i < num_children; i++) {
        int status;
        pid_t child = wait(&status);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        } else {
            printf("Child process %d failed\n", (int)child);
            exit(EXIT_FAILURE);
        }
    }
    
    printf("All child processes completed successfully\n");
    exit(EXIT_SUCCESS);
}
