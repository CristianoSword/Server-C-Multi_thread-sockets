// Sistema multi-thread com socket - Versão corrigida
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <semaphore.h>
#include <time.h>
#include <dlfcn.h>      // Para dlopen, dlsym, RTLD_LAZY
#include <dirent.h>     // Para DIR, opendir, readdir, closedir

// 1. Programação concorrente
#define MAX_THREADS 10
#define BUFFER_SIZE 1024

typedef struct {
    int socket_cliente;
    struct sockaddr_in endereco;
    pthread_t thread_id;
    sem_t *semaforo;
} ConexaoCliente;
