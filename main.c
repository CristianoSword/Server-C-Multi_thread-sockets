/**
 * ============================================================================
 * Programa: Sistema Server C - Multi-thread com Socket 
 * Autor: Cristiano Camacho
 * Data: 13/12/2025
 * ============================================================================
 * Descrição:
 *   Sistema avançado de servidor multi-thread com as seguintes funcionalidades:
 *   - Pool de threads gerenciado por semáforos
 *   - Cache LRU (Least Recently Used)
 *   - Sistema de logging assíncrono
 *   - Balanceador de carga round-robin
 *   - Carregamento dinâmico de plugins
 *   - Otimizações com inline assembly
 * ============================================================================
 */

// Sistema multi-thread com socket - VERSAO WINDOWS NATIVA
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_THREADS 10
#define BUFFER_SIZE 1024
#define CACHE_CAPACITY 100
#define LOG_BUFFER_SIZE 1000
#define PORTA_SERVIDOR 9090
#define MAX_PLUGINS 10

// Estruturas de dados
typedef struct {
    SOCKET socket_cliente;
    struct sockaddr_in endereco;
    HANDLE thread_handle;
    HANDLE *semaforo;
} ConexaoCliente;

typedef struct CacheNode {
    char *chave;
    void *dados;
    size_t tamanho_dados;
    time_t timestamp;
    struct CacheNode *proximo;
    struct CacheNode *anterior;
} CacheNode;

typedef struct {
    CacheNode *cabeca;
    CacheNode *cauda;
    int capacidade;
    int tamanho;
    CRITICAL_SECTION mutex;
} LRUCache;

typedef struct {
    FILE *arquivo_log;
    CRITICAL_SECTION mutex_log;
    HANDLE cond_log;
    char **buffer_log;
    int indice_escrita;
    int indice_leitura;
    int tamanho_buffer;
    int rodando;
    HANDLE thread_logger;
} SistemaLog;

typedef struct {
    struct sockaddr_in servidores[5];
    int atual;
    int servidores_ativos;
    int *health_check;
    CRITICAL_SECTION mutex_balanceamento;
} BalanceadorCarga;

typedef void (*PluginInitFunc)(void*);
typedef void (*PluginProcessFunc)(const char*, void*);

typedef struct {
    HMODULE handle;
    PluginInitFunc init;
    PluginProcessFunc process;
    char nome[50];
} Plugin;

typedef struct {
    Plugin plugins[MAX_PLUGINS];
    int total_plugins;
    CRITICAL_SECTION mutex;
} SistemaPlugins;

// Variaveis globais
LRUCache *cache_global = NULL;
SistemaLog *log_global = NULL;
BalanceadorCarga *balanceador_global = NULL;
SistemaPlugins *sistema_plugins_global = NULL;
HANDLE semaforo_pool;
volatile int servidor_rodando = 1;

// Forward declarations
void escrever_log(SistemaLog *log, const char *formato, ...);

// CACHE LRU
LRUCache* criar_cache(int capacidade) {
    LRUCache *cache = (LRUCache*)malloc(sizeof(LRUCache));
    cache->cabeca = NULL;
    cache->cauda = NULL;
    cache->capacidade = capacidade;
    cache->tamanho = 0;
    InitializeCriticalSection(&cache->mutex);
    return cache;
}

void remover_node_cache(LRUCache *cache, CacheNode *node) {
    if (node->anterior) {
        node->anterior->proximo = node->proximo;
    } else {
        cache->cabeca = node->proximo;
    }
    if (node->proximo) {
        node->proximo->anterior = node->anterior;
    } else {
        cache->cauda = node->anterior;
    }
}

void adicionar_no_topo(LRUCache *cache, CacheNode *node) {
    node->proximo = cache->cabeca;
    node->anterior = NULL;
    if (cache->cabeca) {
        cache->cabeca->anterior = node;
    }
    cache->cabeca = node;
    if (!cache->cauda) {
        cache->cauda = node;
    }
}

void* cache_get(LRUCache *cache, const char *chave) {
    EnterCriticalSection(&cache->mutex);
    CacheNode *atual = cache->cabeca;
    while (atual) {
        if (strcmp(atual->chave, chave) == 0) {
            remover_node_cache(cache, atual);
            adicionar_no_topo(cache, atual);
            atual->timestamp = time(NULL);
            void *dados = atual->dados;
            LeaveCriticalSection(&cache->mutex);
            return dados;
        }
        atual = atual->proximo;
    }
    LeaveCriticalSection(&cache->mutex);
    return NULL;
}

void cache_put(LRUCache *cache, const char *chave, void *dados, size_t tamanho) {
    EnterCriticalSection(&cache->mutex);
    CacheNode *atual = cache->cabeca;
    while (atual) {
        if (strcmp(atual->chave, chave) == 0) {
            free(atual->dados);
            atual->dados = malloc(tamanho);
            memcpy(atual->dados, dados, tamanho);
            atual->tamanho_dados = tamanho;
            atual->timestamp = time(NULL);
            remover_node_cache(cache, atual);
            adicionar_no_topo(cache, atual);
            LeaveCriticalSection(&cache->mutex);
            return;
        }
        atual = atual->proximo;
    }
    CacheNode *novo = (CacheNode*)malloc(sizeof(CacheNode));
    novo->chave = _strdup(chave);
    novo->dados = malloc(tamanho);
    memcpy(novo->dados, dados, tamanho);
    novo->tamanho_dados = tamanho;
    novo->timestamp = time(NULL);
    adicionar_no_topo(cache, novo);
    cache->tamanho++;
    if (cache->tamanho > cache->capacidade) {
        CacheNode *remover = cache->cauda;
        remover_node_cache(cache, remover);
        free(remover->chave);
        free(remover->dados);
        free(remover);
        cache->tamanho--;
    }
    LeaveCriticalSection(&cache->mutex);
}

void destruir_cache(LRUCache *cache) {
    EnterCriticalSection(&cache->mutex);
    CacheNode *atual = cache->cabeca;
    while (atual) {
        CacheNode *proximo = atual->proximo;
        free(atual->chave);
        free(atual->dados);
        free(atual);
        atual = proximo;
    }
    LeaveCriticalSection(&cache->mutex);
    DeleteCriticalSection(&cache->mutex);
    free(cache);
}

// SISTEMA DE LOGGING
DWORD WINAPI thread_logger_func(LPVOID arg) {
    SistemaLog *log = (SistemaLog*)arg;
    while (log->rodando || log->indice_leitura != log->indice_escrita) {
        EnterCriticalSection(&log->mutex_log);
        while (log->indice_leitura == log->indice_escrita && log->rodando) {
            LeaveCriticalSection(&log->mutex_log);
            WaitForSingleObject(log->cond_log, 100);
            EnterCriticalSection(&log->mutex_log);
        }
        while (log->indice_leitura != log->indice_escrita) {
            char *mensagem = log->buffer_log[log->indice_leitura];
            if (mensagem) {
                fprintf(log->arquivo_log, "%s", mensagem);
                fflush(log->arquivo_log);
                free(mensagem);
                log->buffer_log[log->indice_leitura] = NULL;
            }
            log->indice_leitura = (log->indice_leitura + 1) % log->tamanho_buffer;
        }
        LeaveCriticalSection(&log->mutex_log);
    }
    return 0;
}

SistemaLog* criar_sistema_log(const char *arquivo) {
    SistemaLog *log = (SistemaLog*)malloc(sizeof(SistemaLog));
    log->arquivo_log = fopen(arquivo, "a");
    if (!log->arquivo_log) {
        perror("Erro ao abrir arquivo de log");
        free(log);
        return NULL;
    }
    InitializeCriticalSection(&log->mutex_log);
    log->cond_log = CreateEvent(NULL, FALSE, FALSE, NULL);
    log->tamanho_buffer = LOG_BUFFER_SIZE;
    log->buffer_log = (char**)calloc(log->tamanho_buffer, sizeof(char*));
    log->indice_escrita = 0;
    log->indice_leitura = 0;
    log->rodando = 1;
    log->thread_logger = CreateThread(NULL, 0, thread_logger_func, log, 0, NULL);
    return log;
}

void escrever_log(SistemaLog *log, const char *formato, ...) {
    char buffer[1024];
    char temp[900];
    time_t agora = time(NULL);
    struct tm tm_info;
    localtime_s(&tm_info, &agora);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", &tm_info);
    va_list args;
    va_start(args, formato);
    vsnprintf(temp, sizeof(temp), formato, args);
    va_end(args);
    snprintf(buffer, sizeof(buffer), "%s %s\n", timestamp, temp);
    EnterCriticalSection(&log->mutex_log);
    int proximo = (log->indice_escrita + 1) % log->tamanho_buffer;
    if (proximo != log->indice_leitura) {
        log->buffer_log[log->indice_escrita] = _strdup(buffer);
        log->indice_escrita = proximo;
        SetEvent(log->cond_log);
    }
    LeaveCriticalSection(&log->mutex_log);
}

void destruir_sistema_log(SistemaLog *log) {
    log->rodando = 0;
    SetEvent(log->cond_log);
    WaitForSingleObject(log->thread_logger, INFINITE);
    CloseHandle(log->thread_logger);
    fclose(log->arquivo_log);
    for (int i = 0; i < log->tamanho_buffer; i++) {
        if (log->buffer_log[i]) {
            free(log->buffer_log[i]);
        }
    }
    free(log->buffer_log);
    CloseHandle(log->cond_log);
    DeleteCriticalSection(&log->mutex_log);
    free(log);
}

// BALANCEADOR DE CARGA
BalanceadorCarga* criar_balanceador() {
    BalanceadorCarga *bal = (BalanceadorCarga*)malloc(sizeof(BalanceadorCarga));
    bal->atual = 0;
    bal->servidores_ativos = 0;
    bal->health_check = (int*)calloc(5, sizeof(int));
    InitializeCriticalSection(&bal->mutex_balanceamento);
    return bal;
}

void adicionar_servidor(BalanceadorCarga *bal, const char *ip, int porta) {
    EnterCriticalSection(&bal->mutex_balanceamento);
    if (bal->servidores_ativos < 5) {
        int idx = bal->servidores_ativos;
        bal->servidores[idx].sin_family = AF_INET;
        bal->servidores[idx].sin_port = htons(porta);
        inet_pton(AF_INET, ip, &bal->servidores[idx].sin_addr);
        bal->health_check[idx] = 1;
        bal->servidores_ativos++;
    }
    LeaveCriticalSection(&bal->mutex_balanceamento);
}

void destruir_balanceador(BalanceadorCarga *bal) {
    free(bal->health_check);
    DeleteCriticalSection(&bal->mutex_balanceamento);
    free(bal);
}

// SISTEMA DE PLUGINS
SistemaPlugins* criar_sistema_plugins() {
    SistemaPlugins *sp = (SistemaPlugins*)malloc(sizeof(SistemaPlugins));
    sp->total_plugins = 0;
    InitializeCriticalSection(&sp->mutex);
    return sp;
}

void registrar_plugin(Plugin *plugin) {
    EnterCriticalSection(&sistema_plugins_global->mutex);
    if (sistema_plugins_global->total_plugins < MAX_PLUGINS) {
        sistema_plugins_global->plugins[sistema_plugins_global->total_plugins] = *plugin;
        sistema_plugins_global->total_plugins++;
        escrever_log(log_global, "Plugin registrado: %s", plugin->nome);
    }
    LeaveCriticalSection(&sistema_plugins_global->mutex);
}

void carregar_plugins(const char *diretorio) {
    WIN32_FIND_DATAA findData;
    char searchPath[512];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.dll", diretorio);
    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        escrever_log(log_global, "Diretorio de plugins nao encontrado: %s", diretorio);
        return;
    }
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s\\%s", diretorio, findData.cFileName);
            HMODULE handle = LoadLibraryA(caminho);
            if (handle) {
                Plugin plugin;
                plugin.handle = handle;
                plugin.init = (PluginInitFunc)GetProcAddress(handle, "plugin_init");
                plugin.process = (PluginProcessFunc)GetProcAddress(handle, "plugin_process");
                strncpy_s(plugin.nome, sizeof(plugin.nome), findData.cFileName, _TRUNCATE);
                if (plugin.init && plugin.process) {
                    plugin.init(NULL);
                    registrar_plugin(&plugin);
                } else {
                    FreeLibrary(handle);
                }
            } else {
                escrever_log(log_global, "Erro ao carregar %s", caminho);
            }
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
}

void executar_plugins(const char *dados) {
    if (!sistema_plugins_global) return;
    EnterCriticalSection(&sistema_plugins_global->mutex);
    for (int i = 0; i < sistema_plugins_global->total_plugins; i++) {
        Plugin *p = &sistema_plugins_global->plugins[i];
        if (p->process) {
            p->process(dados, NULL);
        }
    }
    LeaveCriticalSection(&sistema_plugins_global->mutex);
}

// MULTIPLICACAO OTIMIZADA
int multiplicacao_otimizada(int a, int b) {
    return a * b;
}

// PROCESSAMENTO DE REQUISICOES
void processar_requisicao_distribuida(const char *buffer, ConexaoCliente *conexao) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(conexao->endereco.sin_addr), ip_str, INET_ADDRSTRLEN);
    escrever_log(log_global, "Processando requisicao de %s:%d", ip_str, ntohs(conexao->endereco.sin_port));
    void *dados_cache = cache_get(cache_global, buffer);
    char resposta[BUFFER_SIZE];
    if (dados_cache) {
        snprintf(resposta, sizeof(resposta), 
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                "Resposta do CACHE: %s\n", (char*)dados_cache);
        escrever_log(log_global, "Cache HIT: %s", buffer);
    } else {
        snprintf(resposta, sizeof(resposta),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                "Processado: %s\nMultiplicacao 7x8 = %d\n",
                buffer, multiplicacao_otimizada(7, 8));
        cache_put(cache_global, buffer, resposta, strlen(resposta) + 1);
        escrever_log(log_global, "Cache MISS: %s", buffer);
    }
    if (sistema_plugins_global && sistema_plugins_global->total_plugins > 0) {
        executar_plugins(buffer);
    }
    send(conexao->socket_cliente, resposta, (int)strlen(resposta), 0);
}

// POOL DE THREADS
DWORD WINAPI gerenciador_conexoes(LPVOID arg) {
    ConexaoCliente *conexao = (ConexaoCliente*)arg;
    WaitForSingleObject(*conexao->semaforo, INFINITE);
    char buffer[BUFFER_SIZE];
    int bytes_recebidos = recv(conexao->socket_cliente, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_recebidos > 0) {
        buffer[bytes_recebidos] = '\0';
        processar_requisicao_distribuida(buffer, conexao);
    } else if (bytes_recebidos == 0) {
        escrever_log(log_global, "Cliente desconectou");
    } else {
        escrever_log(log_global, "Erro ao receber dados");
    }
    ReleaseSemaphore(*conexao->semaforo, 1, NULL);
    closesocket(conexao->socket_cliente);
    free(conexao);
    return 0;
}

// SERVIDOR SOCKET
DWORD WINAPI servidor_socket(LPVOID arg) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup falhou\n");
        return 1;
    }
    
    SOCKET socket_servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_servidor == INVALID_SOCKET) {
        printf("Erro ao criar socket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    
    // Permite reusar a porta
    int opt = 1;
    setsockopt(socket_servidor, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    struct sockaddr_in endereco_servidor;
    endereco_servidor.sin_family = AF_INET;
    endereco_servidor.sin_addr.s_addr = INADDR_ANY;
    endereco_servidor.sin_port = htons(PORTA_SERVIDOR);
    
    if (bind(socket_servidor, (struct sockaddr*)&endereco_servidor, sizeof(endereco_servidor)) == SOCKET_ERROR) {
        printf("Erro no bind (porta %d pode estar em uso): %d\n", PORTA_SERVIDOR, WSAGetLastError());
        printf("Tente fechar outros programas usando a porta ou mude PORTA_SERVIDOR\n");
        closesocket(socket_servidor);
        WSACleanup();
        return 1;
    }
    
    if (listen(socket_servidor, 10) == SOCKET_ERROR) {
        printf("Erro no listen: %d\n", WSAGetLastError());
        closesocket(socket_servidor);
        WSACleanup();
        return 1;
    }
    
    escrever_log(log_global, "Servidor rodando na porta %d", PORTA_SERVIDOR);
    printf("Servidor rodando na porta %d\n", PORTA_SERVIDOR);
    printf("Teste com: curl http://localhost:%d\n", PORTA_SERVIDOR);
    
    while (servidor_rodando) {
        struct sockaddr_in endereco_cliente;
        int tamanho_endereco = sizeof(endereco_cliente);
        
        SOCKET socket_cliente = accept(socket_servidor, (struct sockaddr*)&endereco_cliente, &tamanho_endereco);
        
        if (socket_cliente == INVALID_SOCKET) {
            if (servidor_rodando) {
                escrever_log(log_global, "Erro no accept: %d", WSAGetLastError());
            }
            continue;
        }
        
        ConexaoCliente *conexao = (ConexaoCliente*)malloc(sizeof(ConexaoCliente));
        conexao->socket_cliente = socket_cliente;
        conexao->endereco = endereco_cliente;
        conexao->semaforo = &semaforo_pool;
        conexao->thread_handle = CreateThread(NULL, 0, gerenciador_conexoes, conexao, 0, NULL);
        CloseHandle(conexao->thread_handle);
    }
    
    closesocket(socket_servidor);
    WSACleanup();
    return 0;
}

// SIGNAL HANDLER
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        printf("\n\nEncerrando servidor...\n");
        servidor_rodando = 0;
        return TRUE;
    }
    return FALSE;
}

// MAIN
int main() {
    printf("==============================================\n");
    printf("  SISTEMA MULTI-THREAD COMPLETO - WINDOWS\n");
    printf("==============================================\n\n");
    
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    semaforo_pool = CreateSemaphore(NULL, MAX_THREADS, MAX_THREADS, NULL);
    log_global = criar_sistema_log("servidor.log");
    if (!log_global) {
        fprintf(stderr, "Falha ao inicializar sistema de log\n");
        return 1;
    }
    Sleep(500);
    escrever_log(log_global, "Sistema iniciado");
    cache_global = criar_cache(CACHE_CAPACITY);
    escrever_log(log_global, "Cache LRU criado com capacidade %d", CACHE_CAPACITY);
    balanceador_global = criar_balanceador();
    adicionar_servidor(balanceador_global, "127.0.0.1", 8081);
    adicionar_servidor(balanceador_global, "127.0.0.1", 8082);
    escrever_log(log_global, "Balanceador de carga configurado");
    sistema_plugins_global = criar_sistema_plugins();
    carregar_plugins("./plugins");
    escrever_log(log_global, "Sistema de plugins inicializado");
    int teste_mult = multiplicacao_otimizada(12, 15);
    printf("Teste multiplicacao otimizada: 12 x 15 = %d\n", teste_mult);
    escrever_log(log_global, "Multiplicacao otimizada: 12 x 15 = %d", teste_mult);
    char dado_teste[] = "Hello Cache!";
    cache_put(cache_global, "teste1", dado_teste, strlen(dado_teste) + 1);
    char *recuperado = (char*)cache_get(cache_global, "teste1");
    printf("Teste cache: %s\n", recuperado ? recuperado : "FALHOU");
    printf("\n==============================================\n");
    printf("Todos os sistemas inicializados!\n");
    printf("==============================================\n\n");
    HANDLE thread_servidor = CreateThread(NULL, 0, servidor_socket, NULL, 0, NULL);
    WaitForSingleObject(thread_servidor, INFINITE);
    CloseHandle(thread_servidor);
    printf("\nLimpando recursos...\n");
    destruir_cache(cache_global);
    destruir_balanceador(balanceador_global);
    destruir_sistema_log(log_global);
    CloseHandle(semaforo_pool);
    printf("Sistema encerrado com sucesso!\n");
    return 0;
}
