#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>

/* Includes para a chamada de sistema direta */
#include <lib.h>
#include <minix/callnr.h>

#define SEC(tv) (tv.tv_sec + tv.tv_usec/1e6)

int main(int argc, char **argv) {
    message m;

    /*
     * SOLUÇÃO DEFINITIVA: O processo pai define sua própria política ANTES
     * de criar qualquer filho, eliminando a condição de corrida.
     * O valor 19 é um 'nice' válido que descobrimos que o PM traduz
     * para a fila de prioridade 6, que é o nosso gatilho FCFS.
     */
    printf("Definindo política FCFS para o processo pai %d...\n", getpid());
    m.m2_i1 = getpid(); // Alvo: este próprio processo
    m.m2_i2 = 19;       // O valor de 'nice' que gera a prioridade 6
    if (_syscall(PM_PROC_NR, 23, &m) != 0) { // 23 é o número da chamada NICE
        perror("syscall NICE falhou");
        return -1;
    }
    printf("Política definida com sucesso. Criando processos filhos...\n");
    
    struct timeval p_start, p_end, p_time;
    int *pid;
    unsigned long int x=1;
    int num, nproc, io_ops, cpu_ops;
    long int i=0;

    if (argc != 4) {
        printf("Uso: %s <num_procs> <IO_ops> <CPU_ops>\n", argv[0]);
        return 0;
    }
    nproc = atoi(argv[1]);
    io_ops = atoi(argv[2]);
    cpu_ops = atoi(argv[3]);
    pid = (int *)calloc(nproc, sizeof(int));

    for(num=0; num<nproc; num++) {
        pid[num]=fork();
        if(pid[num]==0) {
            // Lógica dos filhos permanece a mesma
            if((num % 2) == 0) { // IO-bound
                gettimeofday(&p_start, NULL);
                for(i=0; i<io_ops; i++){
                    // Vamos remover o printf para não poluir a saída do teste
                    // fprintf(stderr, "Proc:%d i=%ld\n", num, i);
                    // fflush(stderr);
                }
                gettimeofday(&p_end, NULL);
                timersub(&p_end, &p_start, &p_time);
                printf("IO\t %d\t %g\n",num, SEC(p_time));
            } else { // CPU-bound
                gettimeofday(&p_start, NULL);
                for(i=0; i<cpu_ops; i++){
                    x = (x << 4) - (x << 4);
                }
                gettimeofday(&p_end, NULL);
                timersub(&p_end, &p_start, &p_time);
                printf("CPU\t %d\t %g\n",num, SEC(p_time));
            }
            exit(0);
        }
    }

    for(i=0; i<nproc; i++) {
        wait(NULL);
    }
    return 0;
}
