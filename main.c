/**
 * ============================================================================
 * Program: Server System C - Multi-thread with Socket 
 * Author: Cristiano Camacho
 * Date: 12/14/2025
 * ============================================================================
 * Description:
 *   Advanced multi-thread server system with the following features:
 *   - Thread pool managed by semaphores
 *   - LRU (Least Recently Used) Cache
 *   - Asynchronous logging system
 *   - Round-robin load balancer
 *   - Dynamic plugin loading
 *   - Optimizations with inline assembly
 * ============================================================================
 */

// Multi-thread system with socket - NATIVE WINDOWS VERSION
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
#define SERVER_PORT 9090
#define MAX_PLUGINS 10

// Data structures
typedef struct {
    SOCKET client_socket;
    struct sockaddr_in address;
    HANDLE thread_handle;
    HANDLE *semaphore;
} ClientConnection;

typedef struct CacheNode {
    char *key;
    void *data;
    size_t data_size;
    time_t timestamp;
    struct CacheNode *next;
    struct CacheNode *previous;
} CacheNode;

typedef struct {
    CacheNode *head;
    CacheNode *tail;
    int capacity;
    int size;
    CRITICAL_SECTION mutex;
} LRUCache;

typedef struct {
    FILE *log_file;
    CRITICAL_SECTION log_mutex;
    HANDLE log_cond;
    char **log_buffer;
    int write_index;
    int read_index;
    int buffer_size;
    int running;
    HANDLE logger_thread;
} LogSystem;

typedef struct {
    struct sockaddr_in servers[5];
    int current;
    int active_servers;
    int *health_check;
    CRITICAL_SECTION balancing_mutex;
} LoadBalancer;

typedef void (*PluginInitFunc)(void*);
typedef void (*PluginProcessFunc)(const char*, void*);

typedef struct {
    HMODULE handle;
    PluginInitFunc init;
    PluginProcessFunc process;
    char name[50];
} Plugin;

typedef struct {
    Plugin plugins[MAX_PLUGINS];
    int total_plugins;
    CRITICAL_SECTION mutex;
} PluginSystem;

// Global variables
LRUCache *global_cache = NULL;
LogSystem *global_log = NULL;
LoadBalancer *global_balancer = NULL;
PluginSystem *global_plugin_system = NULL;
HANDLE pool_semaphore;
volatile int server_running = 1;

// Forward declarations
void write_log(LogSystem *log, const char *format, ...);

// LRU CACHE
LRUCache* create_cache(int capacity) {
    LRUCache *cache = (LRUCache*)malloc(sizeof(LRUCache));
    cache->head = NULL;
    cache->tail = NULL;
    cache->capacity = capacity;
    cache->size = 0;
    InitializeCriticalSection(&cache->mutex);
    return cache;
}

void remove_cache_node(LRUCache *cache, CacheNode *node) {
    if (node->previous) {
        node->previous->next = node->next;
    } else {
        cache->head = node->next;
    }
    if (node->next) {
        node->next->previous = node->previous;
    } else {
        cache->tail = node->previous;
    }
}

void add_to_top(LRUCache *cache, CacheNode *node) {
    node->next = cache->head;
    node->previous = NULL;
    if (cache->head) {
        cache->head->previous = node;
    }
    cache->head = node;
    if (!cache->tail) {
        cache->tail = node;
    }
}

void* cache_get(LRUCache *cache, const char *key) {
    EnterCriticalSection(&cache->mutex);
    CacheNode *current = cache->head;
    while (current) {
        if (strcmp(current->key, key) == 0) {
            remove_cache_node(cache, current);
            add_to_top(cache, current);
            current->timestamp = time(NULL);
            void *data = current->data;
            LeaveCriticalSection(&cache->mutex);
            return data;
        }
        current = current->next;
    }
    LeaveCriticalSection(&cache->mutex);
    return NULL;
}

void cache_put(LRUCache *cache, const char *key, void *data, size_t size) {
    EnterCriticalSection(&cache->mutex);
    CacheNode *current = cache->head;
    while (current) {
        if (strcmp(current->key, key) == 0) {
            free(current->data);
            current->data = malloc(size);
            memcpy(current->data, data, size);
            current->data_size = size;
            current->timestamp = time(NULL);
            remove_cache_node(cache, current);
            add_to_top(cache, current);
            LeaveCriticalSection(&cache->mutex);
            return;
        }
        current = current->next;
    }
    CacheNode *new_node = (CacheNode*)malloc(sizeof(CacheNode));
    new_node->key = _strdup(key);
    new_node->data = malloc(size);
    memcpy(new_node->data, data, size);
    new_node->data_size = size;
    new_node->timestamp = time(NULL);
    add_to_top(cache, new_node);
    cache->size++;
    if (cache->size > cache->capacity) {
        CacheNode *remove = cache->tail;
        remove_cache_node(cache, remove);
        free(remove->key);
        free(remove->data);
        free(remove);
        cache->size--;
    }
    LeaveCriticalSection(&cache->mutex);
}

void destroy_cache(LRUCache *cache) {
    EnterCriticalSection(&cache->mutex);
    CacheNode *current = cache->head;
    while (current) {
        CacheNode *next = current->next;
        free(current->key);
        free(current->data);
        free(current);
        current = next;
    }
    LeaveCriticalSection(&cache->mutex);
    DeleteCriticalSection(&cache->mutex);
    free(cache);
}

// LOGGING SYSTEM
DWORD WINAPI logger_thread_func(LPVOID arg) {
    LogSystem *log = (LogSystem*)arg;
    while (log->running || log->read_index != log->write_index) {
        EnterCriticalSection(&log->log_mutex);
        while (log->read_index == log->write_index && log->running) {
            LeaveCriticalSection(&log->log_mutex);
            WaitForSingleObject(log->log_cond, 100);
            EnterCriticalSection(&log->log_mutex);
        }
        while (log->read_index != log->write_index) {
            char *message = log->log_buffer[log->read_index];
            if (message) {
                fprintf(log->log_file, "%s", message);
                fflush(log->log_file);
                free(message);
                log->log_buffer[log->read_index] = NULL;
            }
            log->read_index = (log->read_index + 1) % log->buffer_size;
        }
        LeaveCriticalSection(&log->log_mutex);
    }
    return 0;
}

LogSystem* create_log_system(const char *file) {
    LogSystem *log = (LogSystem*)malloc(sizeof(LogSystem));
    log->log_file = fopen(file, "a");
    if (!log->log_file) {
        perror("Error opening log file");
        free(log);
        return NULL;
    }
    InitializeCriticalSection(&log->log_mutex);
    log->log_cond = CreateEvent(NULL, FALSE, FALSE, NULL);
    log->buffer_size = LOG_BUFFER_SIZE;
    log->log_buffer = (char**)calloc(log->buffer_size, sizeof(char*));
    log->write_index = 0;
    log->read_index = 0;
    log->running = 1;
    log->logger_thread = CreateThread(NULL, 0, logger_thread_func, log, 0, NULL);
    return log;
}

void write_log(LogSystem *log, const char *format, ...) {
    char buffer[1024];
    char temp[900];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_s(&tm_info, &now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", &tm_info);
    va_list args;
    va_start(args, format);
    vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);
    snprintf(buffer, sizeof(buffer), "%s %s\n", timestamp, temp);
    EnterCriticalSection(&log->log_mutex);
    int next = (log->write_index + 1) % log->buffer_size;
    if (next != log->read_index) {
        log->log_buffer[log->write_index] = _strdup(buffer);
        log->write_index = next;
        SetEvent(log->log_cond);
    }
    LeaveCriticalSection(&log->log_mutex);
}

void destroy_log_system(LogSystem *log) {
    log->running = 0;
    SetEvent(log->log_cond);
    WaitForSingleObject(log->logger_thread, INFINITE);
    CloseHandle(log->logger_thread);
    fclose(log->log_file);
    for (int i = 0; i < log->buffer_size; i++) {
        if (log->log_buffer[i]) {
            free(log->log_buffer[i]);
        }
    }
    free(log->log_buffer);
    CloseHandle(log->log_cond);
    DeleteCriticalSection(&log->log_mutex);
    free(log);
}

// LOAD BALANCER
LoadBalancer* create_balancer() {
    LoadBalancer *bal = (LoadBalancer*)malloc(sizeof(LoadBalancer));
    bal->current = 0;
    bal->active_servers = 0;
    bal->health_check = (int*)calloc(5, sizeof(int));
    InitializeCriticalSection(&bal->balancing_mutex);
    return bal;
}

void add_server(LoadBalancer *bal, const char *ip, int port) {
    EnterCriticalSection(&bal->balancing_mutex);
    if (bal->active_servers < 5) {
        int idx = bal->active_servers;
        bal->servers[idx].sin_family = AF_INET;
        bal->servers[idx].sin_port = htons(port);
        inet_pton(AF_INET, ip, &bal->servers[idx].sin_addr);
        bal->health_check[idx] = 1;
        bal->active_servers++;
    }
    LeaveCriticalSection(&bal->balancing_mutex);
}

void destroy_balancer(LoadBalancer *bal) {
    free(bal->health_check);
    DeleteCriticalSection(&bal->balancing_mutex);
    free(bal);
}

// PLUGIN SYSTEM
PluginSystem* create_plugin_system() {
    PluginSystem *ps = (PluginSystem*)malloc(sizeof(PluginSystem));
    ps->total_plugins = 0;
    InitializeCriticalSection(&ps->mutex);
    return ps;
}

void register_plugin(Plugin *plugin) {
    EnterCriticalSection(&global_plugin_system->mutex);
    if (global_plugin_system->total_plugins < MAX_PLUGINS) {
        global_plugin_system->plugins[global_plugin_system->total_plugins] = *plugin;
        global_plugin_system->total_plugins++;
        write_log(global_log, "Plugin registered: %s", plugin->name);
    }
    LeaveCriticalSection(&global_plugin_system->mutex);
}

void load_plugins(const char *directory) {
    WIN32_FIND_DATAA findData;
    char searchPath[512];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.dll", directory);
    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        write_log(global_log, "Plugin directory not found: %s", directory);
        return;
    }
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char path[512];
            snprintf(path, sizeof(path), "%s\\%s", directory, findData.cFileName);
            HMODULE handle = LoadLibraryA(path);
            if (handle) {
                Plugin plugin;
                plugin.handle = handle;
                plugin.init = (PluginInitFunc)GetProcAddress(handle, "plugin_init");
                plugin.process = (PluginProcessFunc)GetProcAddress(handle, "plugin_process");
                strncpy_s(plugin.name, sizeof(plugin.name), findData.cFileName, _TRUNCATE);
                if (plugin.init && plugin.process) {
                    plugin.init(NULL);
                    register_plugin(&plugin);
                } else {
                    FreeLibrary(handle);
                }
            } else {
                write_log(global_log, "Error loading %s", path);
            }
        }
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);
}

void execute_plugins(const char *data) {
    if (!global_plugin_system) return;
    EnterCriticalSection(&global_plugin_system->mutex);
    for (int i = 0; i < global_plugin_system->total_plugins; i++) {
        Plugin *p = &global_plugin_system->plugins[i];
        if (p->process) {
            p->process(data, NULL);
        }
    }
    LeaveCriticalSection(&global_plugin_system->mutex);
}

// OPTIMIZED MULTIPLICATION
int optimized_multiplication(int a, int b) {
    return a * b;
}

// REQUEST PROCESSING
void process_distributed_request(const char *buffer, ClientConnection *connection) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(connection->address.sin_addr), ip_str, INET_ADDRSTRLEN);
    write_log(global_log, "Processing request from %s:%d", ip_str, ntohs(connection->address.sin_port));
    void *cache_data = cache_get(global_cache, buffer);
    char response[BUFFER_SIZE];
    if (cache_data) {
        snprintf(response, sizeof(response), 
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                "Response from CACHE: %s\n", (char*)cache_data);
        write_log(global_log, "Cache HIT: %s", buffer);
    } else {
        snprintf(response, sizeof(response),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                "Processed: %s\nMultiplication 7x8 = %d\n",
                buffer, optimized_multiplication(7, 8));
        cache_put(global_cache, buffer, response, strlen(response) + 1);
        write_log(global_log, "Cache MISS: %s", buffer);
    }
    if (global_plugin_system && global_plugin_system->total_plugins > 0) {
        execute_plugins(buffer);
    }
    send(connection->client_socket, response, (int)strlen(response), 0);
}

// THREAD POOL
DWORD WINAPI connection_manager(LPVOID arg) {
    ClientConnection *connection = (ClientConnection*)arg;
    WaitForSingleObject(*connection->semaphore, INFINITE);
    char buffer[BUFFER_SIZE];
    int bytes_received = recv(connection->client_socket, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        process_distributed_request(buffer, connection);
    } else if (bytes_received == 0) {
        write_log(global_log, "Client disconnected");
    } else {
        write_log(global_log, "Error receiving data");
    }
    ReleaseSemaphore(*connection->semaphore, 1, NULL);
    closesocket(connection->client_socket);
    free(connection);
    return 0;
}

// SOCKET SERVER
DWORD WINAPI socket_server(LPVOID arg) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
    
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        printf("Error creating socket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    
    // Allow port reuse
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(SERVER_PORT);
    
    if (bind(server_socket, (struct sockaddr*)&server_address, sizeof(server_address)) == SOCKET_ERROR) {
        printf("Bind error (port %d may be in use): %d\n", SERVER_PORT, WSAGetLastError());
        printf("Try closing other programs using the port or change SERVER_PORT\n");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    
    if (listen(server_socket, 10) == SOCKET_ERROR) {
        printf("Listen error: %d\n", WSAGetLastError());
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    
    write_log(global_log, "Server running on port %d", SERVER_PORT);
    printf("Server running on port %d\n", SERVER_PORT);
    printf("Test with: curl http://localhost:%d\n", SERVER_PORT);
    
    while (server_running) {
        struct sockaddr_in client_address;
        int address_size = sizeof(client_address);
        
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_address, &address_size);
        
        if (client_socket == INVALID_SOCKET) {
            if (server_running) {
                write_log(global_log, "Accept error: %d", WSAGetLastError());
            }
            continue;
        }
        
        ClientConnection *connection = (ClientConnection*)malloc(sizeof(ClientConnection));
        connection->client_socket = client_socket;
        connection->address = client_address;
        connection->semaphore = &pool_semaphore;
        connection->thread_handle = CreateThread(NULL, 0, connection_manager, connection, 0, NULL);
        CloseHandle(connection->thread_handle);
    }
    
    closesocket(server_socket);
    WSACleanup();
    return 0;
}

// SIGNAL HANDLER
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT) {
        printf("\n\nShutting down server...\n");
        server_running = 0;
        return TRUE;
    }
    return FALSE;
}

// MAIN
int main() {
    printf("==============================================\n");
    printf("  COMPLETE MULTI-THREAD SYSTEM - WINDOWS\n");
    printf("==============================================\n\n");
    
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    pool_semaphore = CreateSemaphore(NULL, MAX_THREADS, MAX_THREADS, NULL);
    global_log = create_log_system("server.log");
    if (!global_log) {
        fprintf(stderr, "Failed to initialize log system\n");
        return 1;
    }
    Sleep(500);
    write_log(global_log, "System started");
    global_cache = create_cache(CACHE_CAPACITY);
    write_log(global_log, "LRU Cache created with capacity %d", CACHE_CAPACITY);
    global_balancer = create_balancer();
    add_server(global_balancer, "127.0.0.1", 8081);
    add_server(global_balancer, "127.0.0.1", 8082);
    write_log(global_log, "Load balancer configured");
    global_plugin_system = create_plugin_system();
    load_plugins("./plugins");
    write_log(global_log, "Plugin system initialized");
    int test_mult = optimized_multiplication(12, 15);
    printf("Optimized multiplication test: 12 x 15 = %d\n", test_mult);
    write_log(global_log, "Optimized multiplication: 12 x 15 = %d", test_mult);
    char test_data[] = "Hello Cache!";
    cache_put(global_cache, "test1", test_data, strlen(test_data) + 1);
    char *retrieved = (char*)cache_get(global_cache, "test1");
    printf("Cache test: %s\n", retrieved ? retrieved : "FAILED");
    printf("\n==============================================\n");
    printf("All systems initialized!\n");
    printf("==============================================\n\n");
    HANDLE server_thread = CreateThread(NULL, 0, socket_server, NULL, 0, NULL);
    WaitForSingleObject(server_thread, INFINITE);
    CloseHandle(server_thread);
    printf("\nCleaning up resources...\n");
    destroy_cache(global_cache);
    destroy_balancer(global_balancer);
    destroy_log_system(global_log);
    CloseHandle(pool_semaphore);
    printf("System shut down successfully!\n");
    return 0;
}
