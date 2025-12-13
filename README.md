# Multi-thread com Sockets

Sistema de servidor multi-thread em C demonstrando conceitos avançados de programação concorrente e comunicação em rede.

## 📌 O que tem aqui

- **Pool de Threads** - Gerenciamento eficiente de múltiplas conexões simultâneas
- **Semáforos** - Sincronização entre threads para evitar condições de corrida
- **Sockets TCP** - Comunicação em rede cliente-servidor
- **Cache LRU** - Sistema de cache com política Least Recently Used
- **Logging Assíncrono** - Registro de eventos sem bloquear operações principais
- **Balanceador de Carga** - Distribuição de requisições entre múltiplos servidores
- **Sistema de Plugins** - Carregamento dinâmico de bibliotecas (.so)
- **Inline Assembly** - Otimizações de baixo nível

## 🚀 Compilação

```bash
gcc -pthread -ldl -o servidor sistema_multithread.c
```

## ▶️ Execução

```bash
./servidor
```

## 🔧 Requisitos

- GCC ou Clang
- Linux/Unix (para sockets POSIX e pthreads)
- Biblioteca pthread
- Biblioteca dl (dynamic linking)

## 💡 Conceitos Importantes

**Concorrência** - Múltiplas threads processando requisições ao mesmo tempo

**Sincronização** - Uso de semáforos e mutex para coordenar acesso a recursos compartilhados

**Sockets** - Comunicação entre processos através da rede

**Dynamic Loading** - Carregamento de código em tempo de execução

## 📝 Estrutura do Código

```
ConexaoCliente    → Estrutura para cada cliente conectado
gerenciador_conexoes → Thread que processa requisições
LRUCache          → Sistema de cache otimizado
SistemaLog        → Logger assíncrono thread-safe
BalanceadorCarga  → Distribuição de carga entre servidores
carregar_plugins  → Sistema de extensão via plugins
```

## ⚠️ Nota

Este é um projeto educacional demonstrando técnicas avançadas de programação em C. Para uso em produção, seria necessário adicionar tratamento de erros mais robusto e testes de estresse.

---

## 📄 Licença

MIT License - Livre para uso, modificação e distribuição.
