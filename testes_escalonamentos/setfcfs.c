#include <lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <minix/rs.h>

int main(int argc, char *argv[])
{
    int pid, ret;
    message m;

    if (argc != 2) {
        printf("Uso: %s <pid>\n", argv[0]);
        return 1;
    }

    pid = atoi(argv[1]);

    /* Esta é uma chamada de sistema de baixo nível para o PM */
    /* que irá resultar em uma chamada para do_nice no sched */
    m.m2_i1 = pid;
    m.m2_i2 = 99; // Nosso número mágico!
    
    ret = _syscall(PM_PROC_NR, NICE, &m);
    
    if (ret == 0) {
        printf("Sinal FCFS enviado para o processo %d.\n", pid);
    } else {
        printf("Erro ao enviar sinal para o processo %d: %d\n", pid, ret);
    }
    
    return ret;
}
