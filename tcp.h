#ifndef TCP_HEADER
#define TCP_HEADER

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/types.h>

#ifndef TCP_THREAD_COUNT
#define TCP_THREAD_COUNT  256
#define TCP__THREAD_COUNT tcp__num_cores(TCP_THREAD_COUNT)
#else
#define TCP__THREAD_COUNT TCP_THREAD_COUNT
#endif


typedef struct TcpServer {
    int      fd;
    uint32_t host;
    uint16_t port;
    uint16_t backlog;

#if TCP_THREAD_COUNT > 0
    int thread_count;
    int next_thread;
    int thread_fds[TCP_THREAD_COUNT];
    pthread_t threads[TCP_THREAD_COUNT];
#endif
} TcpServer;


typedef struct TcpClient {
    int fd;
    uint32_t host;
    uint16_t port;
} TcpClient;


typedef struct TcpContext {
    TcpClient client;
    TcpServer server;
    void (*serve)(struct TcpContext*);
} TcpContext;


typedef enum TcpClientStatus {
    TCP_CLIENT_ERROR = -1,
    TCP_CLIENT_REQUESTED_SHUTDOWN = 0,
    TCP_CLIENT_CONNECTED = 1,
} TcpClientStatus;


TcpServer tcp_server(const char* host, uint16_t port, uint16_t backlog);
TcpClient tcp_accept(TcpServer* server, void (*serve)(TcpContext*));
ssize_t   tcp_read(TcpClient* client, char* buffer, size_t bytes);
ssize_t   tcp_write(TcpClient* client, char* buffer, size_t bytes);
void      tcp_close(TcpServer* server);

int  tcp_shutdown_requested(void);
void tcp_request_shutdown(TcpClient client);

const char* tcp_server_error(TcpServer server);
const char* tcp_client_error(TcpClient client);
TcpClientStatus tcp_client_status(TcpClient client);



#endif



#ifdef TCP_IMPLEMENTATION


_Thread_local int thread_id = 0;
static pthread_t g_main_thread;
static bool g_termination_signal_sent = false;


#if !defined(TCP_LOG)
#define TCP_LOG(tid, cid, message, ...)
#endif

#if defined(TCP_STACK_SIZE)
#define COROUTINE_STACK_SIZE (TCP_STACK_SIZE)
#endif

#if defined(TCP_MAX_COROUTINES)
#define COROUTINE_MAX_COUNT (TCP_MAX_COROUTINES)
#endif

#if defined(tcp_stack_allocate) || defined(tcp_stack_deallocate)
#define coroutine_stack_allocate   tcp_stack_allocate
#define coroutine_stack_deallocate tcp_stack_deallocate
#else
#define COROUTINE_STACK_MMAP
#endif

#define COROUTINE_THREAD_COUNT TCP_THREAD_COUNT
#define COROUTINE_LOG(id, message, ...) TCP_LOG(thread_id, id, message, __VA_ARGS__)
#define COROUTINE_IMPLEMENTATION
#include "coroutine.h"

#include <sys/socket.h>
#include <sys/fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/sysctl.h>


static void tcp__on_client_disconnected(void* stack, size_t size) {
    void** top = (void*)((char*)stack + size + sizeof(TcpContext));

    TcpContext* context = (TcpContext*)top - 1;
    TcpClient*  client  = &context->client;

    close(client->fd);
}


#if TCP_THREAD_COUNT > 0
static void tcp__shutdown_signal_handler(int sig) {
    assert(sig == SIGUSR1);
    assert(thread_id == 0);
    assert(coroutine_id() == 0);

    TCP_LOG(thread_id, coroutine_id(), "Shutdown signal received%s", "");

    // NOTE: The server might be sleeping, so we use this to ensure it's
    //       set to active and can proceed with the shutdown.
    coroutine_wake_up(0);
}


static void* tcp__worker_function(void* arg) {
    int fd = (ssize_t)arg & 0xFFFFFFFF;
    thread_id = (ssize_t)arg >> 32;

    TcpContext context = { 0 };
    while (true) {
        coroutine_wait_read(fd);

        // Woken up by explicit shutdown request
        if (tcp_shutdown_requested())
            goto terminate;

        // TODO: Error handling
        ssize_t bytes_read = read(fd, &context, sizeof(context));

        // Woken up by shutdown request from other threads.
        if (bytes_read != sizeof(context))
            goto terminate;

        coroutine_create((void (*)(void *)) context.serve, &context, sizeof(context), tcp__on_client_disconnected);
    }

terminate:
    // TODO: Not atomic.
    if (!g_termination_signal_sent) {
        g_termination_signal_sent = true;
        pthread_kill(g_main_thread, SIGUSR1);
    }
    coroutine_destroy_all();
    return NULL;
}
#endif


int tcp__num_cores(int number_on_error) {
    int num_cores;
    size_t size = sizeof(num_cores);
    return sysctlbyname("hw.logicalcpu", &num_cores, &size, NULL, 0) == 0 ? num_cores : number_on_error;
}



TcpServer tcp_server(const char* host, uint16_t port, uint16_t backlog) {
    int status;

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = host ? inet_addr(host) : INADDR_ANY;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) goto error;

    const int enable = 1;
    status = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (status < 0) goto error;

    status = bind(server_fd, (struct sockaddr*)&server_address, sizeof(server_address));
    if (status < 0) goto error;

    socklen_t server_address_size = sizeof(server_address);
    status = getsockname(server_fd, (struct sockaddr *)&server_address, &server_address_size);
    if (status < 0) goto error;

    status = listen(server_fd, backlog);
    if (status < 0) goto error;

    status = fcntl(server_fd, F_SETFL, fcntl(server_fd, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0) goto error;

    TcpServer result = {
        server_fd,
        server_address.sin_addr.s_addr,
        ntohs(server_address.sin_port),
        backlog,
    };

#if TCP_THREAD_COUNT > 0
    g_main_thread = pthread_self();
    signal(SIGUSR1, tcp__shutdown_signal_handler);

    int read_fds[TCP_THREAD_COUNT] = { 0 };
    int write_fds[TCP_THREAD_COUNT] = { 0 };
    pthread_t threads[TCP_THREAD_COUNT] = { 0 };
    result.thread_count = TCP__THREAD_COUNT;
    for (ssize_t i = 0; i < result.thread_count; ++i) {
        int pipe_fd[2];
        pipe(pipe_fd);

        int read_fd  = pipe_fd[0];
        int write_fd = pipe_fd[1];

        read_fds[i]  = read_fd;
        write_fds[i] = write_fd;

        pthread_t thread;
        pthread_create(&thread, NULL, tcp__worker_function, (void *)((ssize_t)read_fd | ((i+1) << 32)));

        threads[i] = thread;
    }

    memcpy(result.thread_fds, write_fds, result.thread_count * sizeof(*write_fds));
    memcpy(result.threads, threads, result.thread_count * sizeof(*threads));
#endif
    return result;

error:
    status = errno;
    if (server_fd > 0) close(server_fd);
    return (TcpServer){ -status, server_address.sin_addr.s_addr, ntohs(server_address.sin_port), backlog };
}


TcpClient tcp_accept(TcpServer* server, void (*serve)(TcpContext*)) {
    int status;

    coroutine_wait_read(server->fd);
    if (tcp_shutdown_requested()) {
        return (TcpClient) { 0 };
    }

    struct sockaddr_in client_address = { 0 };
    socklen_t client_address_size = sizeof(client_address);

    int client_fd = accept(server->fd, (struct sockaddr*) &client_address, &client_address_size);
    if (client_fd < 0) goto error;

    status = fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0) goto error;

    TcpClient client = { .fd = client_fd, .host = client_address.sin_addr.s_addr, .port = ntohs(client_address.sin_port) };

#if TCP_THREAD_COUNT > 0
    TcpContext context = { client, *server, serve };

    int thread_fd = server->thread_fds[server->next_thread];
    server->next_thread = (server->next_thread + 1) % server->thread_count;

    write(thread_fd, &context, sizeof(context));
#else
    TcpContext context = { client, *server, serve };
    coroutine_create((void (*)(void *)) context.serve, &context, sizeof(context), tcp__on_client_disconnected);
#endif

    return client;

error:
    status = errno;
    if (client_fd > 0) close(client_fd);
    return (TcpClient) { .fd = -status };
}


ssize_t tcp_read(TcpClient* client, char* buffer, size_t bytes) {
    coroutine_wait_read(client->fd);
    return read(client->fd, buffer, bytes);
}


ssize_t tcp_write(TcpClient* client, char* buffer, size_t bytes) {
    coroutine_wait_write(client->fd);
    return write(client->fd, buffer, bytes);
}


void tcp_close(TcpServer* server) {
#if TCP_THREAD_COUNT > 0
    for (int i = 0; i < server->thread_count; ++i) {
        int thread_fd = server->thread_fds[server->next_thread];
        server->next_thread = (server->next_thread + 1) % server->thread_count;
        write(thread_fd, &i, sizeof(i));
    }
#endif

    coroutine_destroy_all();

#if TCP_THREAD_COUNT > 0
    for (int i = 0; i < server->thread_count; ++i) {
        if (pthread_join(server->threads[i], NULL) != 0) {
            perror("pthread_join");
        }
        TCP_LOG(thread_id, coroutine_id(), "Thread %d joined!", i+1);
    }
#endif

    TCP_LOG(thread_id, coroutine_id(), "Terminating server%s", "");
    close(server->fd);
    *server = (TcpServer) { .fd = -1 };
}

static int tcp__shutdown_requested = false;


int tcp_shutdown_requested(void) {
    return tcp__shutdown_requested;
}


void tcp_request_shutdown(TcpClient client) {
    tcp__shutdown_requested = client.fd;
    coroutine_wake_up(0);
}


const char* tcp_server_error(TcpServer server) {
    if (server.fd < 0) {
        int err = -server.fd;
        return strerror(err);
    }
    return NULL;
}


const char* tcp_client_error(TcpClient client) {
    if (client.fd < 0) {
        int err = -client.fd;
        return strerror(err);
    }
    return NULL;
}


TcpClientStatus tcp_client_status(TcpClient client) {
    if (client.fd > 0) return TCP_CLIENT_CONNECTED;
    if (client.fd < 0) return TCP_CLIENT_ERROR;
    return TCP_CLIENT_REQUESTED_SHUTDOWN;
}

#endif
