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


// Declaração forward da função que faltava
void processar_requisicao_distribuida(const char *buffer, ConexaoCliente *conexao);

// 2. Pool de threads
void *gerenciador_conexoes(void *arg) {
    ConexaoCliente *conexao = (ConexaoCliente *)arg;
    
    // Adquire semáforo
    sem_wait(conexao->semaforo);
    
    // Processa requisição
    char buffer[BUFFER_SIZE];
    ssize_t bytes_recebidos = recv(conexao->socket_cliente, 
                                   buffer, BUFFER_SIZE, 0);
    
    if (bytes_recebidos > 0) {
        // Processamento complexo
        buffer[bytes_recebidos] = '\0';
        
        // Lógica de negócio distribuída
        processar_requisicao_distribuida(buffer, conexao);
    }
    
    // Libera semáforo
    sem_post(conexao->semaforo);
    close(conexao->socket_cliente);
    free(conexao);
    
    return NULL;
}


// 3. Cache com algoritmo LRU (Least Recently Used)
typedef struct CacheNode {
    char *chave;
    void *dados;
    time_t timestamp;
    struct CacheNode *proximo;
    struct CacheNode *anterior;
} CacheNode;

typedef struct {
    CacheNode *cabeca;
    CacheNode *cauda;
    int capacidade;
    int tamanho;
    pthread_mutex_t mutex;
} LRUCache;

// 4. Sistema de logging assíncrono
typedef struct {
    FILE *arquivo_log;
    pthread_mutex_t mutex_log;
    pthread_cond_t cond_log;
    char **buffer_log;
    int indice_escrita;
    int indice_leitura;
    pthread_t thread_logger;
} SistemaLog;
// 5. Balanceador de carga
typedef struct {
    struct sockaddr_in servidores[5];
    int atual; // Para round-robin
    int servidores_ativos;
    int *health_check; // Status dos servidores
    pthread_mutex_t mutex_balanceamento;
} BalanceadorCarga;

// 6. Otimização com inline assembly (exemplo)
int multiplicacao_otimizada(int a, int b) {
    int resultado;
    __asm__ (
        "imull %%ebx, %%eax;"
        : "=a" (resultado)
        : "a" (a), "b" (b)
    );
    return resultado;
}

// 7. Sistema de plugins/dynamic loading
typedef void (*PluginInitFunc)(void*);
typedef void (*PluginProcessFunc)(const char*, void*);

typedef struct {
    void *handle;
    PluginInitFunc init;
    PluginProcessFunc process;
    char nome[50];
} Plugin;

// Declaração forward
void registrar_plugin(Plugin *plugin);

void carregar_plugins(const char *diretorio) {
    DIR *dir = opendir(diretorio);
    if (!dir) {
        perror("Erro ao abrir diretório");
        return;
    }
    
    struct dirent *entrada;
    
    while ((entrada = readdir(dir)) != NULL) {
        if (strstr(entrada->d_name, ".so")) {
            char caminho[256];
            snprintf(caminho, sizeof(caminho), "%s/%s", 
                     diretorio, entrada->d_name);
            
            void *handle = dlopen(caminho, RTLD_LAZY);
            if (handle) {
                Plugin *plugin = malloc(sizeof(Plugin));
                plugin->handle = handle;
                plugin->init = dlsym(handle, "plugin_init");
                plugin->process = dlsym(handle, "plugin_process");
                
                if (plugin->init && plugin->process) {
                    plugin->init(NULL); // Inicializa plugin
                    registrar_plugin(plugin);
                }
            } else {
                fprintf(stderr, "Erro ao carregar %s: %s\n", 
                        caminho, dlerror());
            }
        }
    }
    closedir(dir);
}
