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


typedef struct TcpServer {
    int      fd;
    uint32_t host;
    uint16_t port;
    uint16_t backlog;

    int next_thread;
    int thread_fds[3];
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


TcpServer tcp_server(const char* host, uint16_t port, uint16_t backlog);
TcpClient tcp_accept(TcpServer* server, void (*serve)(TcpContext*));
ssize_t   tcp_read(TcpClient* client, char* buffer, size_t bytes);
ssize_t   tcp_write(TcpClient* client, char* buffer, size_t bytes);
void      tcp_close(TcpServer* server);

int  tcp_shutdown_requested();
void tcp_request_shutdown(TcpClient client);

const char* tcp_server_error(TcpServer server);
const char* tcp_client_error(TcpClient client);



#endif



#ifdef TCP_IMPLEMENTATION

#include <pthread.h>

#define COROUTINE_MMAP
#define COROUTINE_IMPLEMENTATION
#include "coroutine.h"
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <arpa/inet.h>
#include <unistd.h>


void tcp__on_client_disconnected(void* stack, size_t size) {
    void** top = (void*)(char*)stack + size + sizeof(TcpContext);

    TcpContext* context = (TcpContext*)top - 1;
    TcpClient* client = &context->client;
    TcpServer* server = &context->server;

    char client_address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client->host), client_address, INET_ADDRSTRLEN);

    printf("[%03d]: Client %d (%s:%d) closed.\n", server->fd, coroutine_id(), client_address, client->port);

    close(client->fd);
}

void* worker_function(void* arg) {
    int fd = (int)(ssize_t)arg;

    TcpContext context = { 0 };
    while (true) {
        coroutine_wait_read(fd);
        ssize_t bytes_read = read(fd, &context, sizeof(context));
        printf("Read completed. Read %ld bytes\n", bytes_read);
        if (bytes_read != sizeof(context)) {
            fprintf(stderr, "Something went wrong while reading from client.\n");
            abort();
        }

        coroutine_create((void*) context.serve, &context, sizeof(context), tcp__on_client_disconnected);
    }
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

    int read_fds[3] = { 0 };
    int write_fds[3] = { 0 };
    for (int i = 0; i < 3; ++i) {
        int pipe_fd[2];
        pipe(pipe_fd);

        int read_fd  = pipe_fd[0];
        int write_fd = pipe_fd[1];

        read_fds[i]  = read_fd;
        write_fds[i] = write_fd;

        pthread_t thread;
        pthread_create(&thread, NULL, worker_function, (void *)(ssize_t)read_fd);

        pthread_detach(thread);
    }

    TcpServer result = {
        server_fd,
        server_address.sin_addr.s_addr,
        ntohs(server_address.sin_port),
        backlog,
        0
    };
    memcpy(result.thread_fds, write_fds, sizeof(write_fds));
    return result;

error:
    status = errno;
    if (server_fd > 0) close(server_fd);
    return (TcpServer){ -status, server_address.sin_addr.s_addr, ntohs(server_address.sin_port), backlog };
}


TcpClient tcp_accept(TcpServer* server, void (*serve)(TcpContext*)) {
    int status;

    coroutine_wait_read(server->fd);

    struct sockaddr_in client_address = { 0 };
    socklen_t client_address_size = sizeof(client_address);

    int client_fd = accept(server->fd, (struct sockaddr*) &client_address, &client_address_size);
    if (client_fd < 0) goto error;

    status = fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);
    if (status < 0) goto error;

    TcpClient client = { .fd = client_fd, .host = client_address.sin_addr.s_addr, .port = ntohs(client_address.sin_port) };
    TcpContext context = { client, *server, serve };

    int thread_fd = server->thread_fds[server->next_thread];
    server->next_thread = (server->next_thread + 1) % 3;

    write(thread_fd, &context, sizeof(context));

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
    close(server->fd);
    *server = (TcpServer) { .fd = -1 };
}

static int tcp__shutdown_requested = false;

int tcp_shutdown_requested() {
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
        // if (err == EAGAIN)
            // return NULL;
        return strerror(err);
    }
    return NULL;
}

#endif
