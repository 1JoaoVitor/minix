#include <stdio.h>
#include <unistd.hh>
#include <minix/syslib.h>
#include <minix/endpoint.h>

int main(int argc, char *argv[]) {
    endpoint_t meu_endpoint;

    // A única coisa que este programa faz é descobrir e imprimir seu próprio endpoint
    if (getprocnr(getpid(), &meu_endpoint) == 0) {
        printf("ANOTE ESTE NUMERO -> Endpoint do processo de teste: %d\n", meu_endpoint);
    } else {
        fprintf(stderr, "Erro ao obter o endpoint.\n");
    }
    return 0;
}
