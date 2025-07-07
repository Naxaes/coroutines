#if defined(LOG)
#define TCP_LOG(tid, cid, message, ...) printf("[%02d-%02d]: " message "\n", tid, cid, __VA_ARGS__)
#endif
#define TCP_IMPLEMENTATION
#include "tcp.h"

#include <assert.h>
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


typedef struct String {
    char* data;
    size_t size;
} String;


String load_html_file(const char* filepath) {
    char* buffer = NULL;
    int fd = -1;
    size_t total_read = 0;
    size_t size = 0;

    fd = open(filepath, O_RDONLY|O_NONBLOCK);
    if (fd < 0) goto error;

    struct stat st;
    if (fstat(fd, &st) != 0) goto error;
    size = st.st_size;

    buffer = malloc(size + 1);
    if (!buffer) goto error;

    while (total_read < size) {
        coroutine_wait_read(fd);
        ssize_t r = read(fd, buffer + total_read, size - total_read);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            goto error;
        }
        if (r == 0) break; // EOF
        total_read += r;
    }

    buffer[total_read] = '\0';
    close(fd);
    return (String) { .data = buffer, .size = total_read };

error:
    if (fd >= 0) close(fd);
    if (buffer) free(buffer);
    return (String) { .data = NULL, .size = 0 };
}



void handle_client(TcpContext* ctx) {
    TcpClient* client = &ctx->client;
    char read_buffer[4096] = { 0 };
    char write_buffer[4096] = { 0 };

    int cid = coroutine_id();
    int tid = thread_id;

    char client_address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client->host, client_address, INET_ADDRSTRLEN);

    while (true) {
        TCP_LOG(tid, cid, "Waiting reading data from client (%s:%d)!", client_address, client->port);
        ssize_t bytes_read = tcp_read(client, read_buffer, sizeof(read_buffer));
        if (bytes_read == 0) {
            TCP_LOG(tid, cid, "Client (%s:%d) disconnected.", client_address, client->port);
            return;
        } else if (bytes_read < 0) {
            TCP_LOG(tid, cid, "Error reading data from client (%s:%d). Exiting...", client_address, client->port);
            perror("handle_client");
            return;
        }
        TCP_LOG(tid, cid, "Read %zd bytes from client (%s:%d)\n'%.*s'\n", bytes_read, client_address, client->port, (int)bytes_read, read_buffer);

        if (strncmp(read_buffer, "exit", sizeof("exit")-1) == 0) {
            break;
        } else if (strncmp(read_buffer, "shutdown", sizeof("shutdown")-1) == 0) {
            tcp_request_shutdown(*client);
            break;
        }

        String index = load_html_file("./resources/index.html");
        if (index.data == NULL) {
            TCP_LOG(tid, cid, "Error loading index.html%s", "");
            return;
        }

        int header_len = snprintf(write_buffer, sizeof(write_buffer),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n", index.size);
        memcpy(write_buffer+header_len, index.data, index.size);
        memcpy(write_buffer+header_len+index.size, read_buffer, bytes_read);

        TCP_LOG(tid, cid, "Waiting writing data to client (%s:%d)!", client_address, client->port);
        ssize_t bytes_written = tcp_write(client, write_buffer, header_len+index.size+bytes_read);
        free(index.data);
        if (bytes_written <= 0) {
            TCP_LOG(tid, cid, "Couldn't write anything to client (%s:%d). Exiting...", client_address, client->port);
            return;
        }
        TCP_LOG(tid, cid, "Wrote %lu bytes to client (%s:%d)", index.size+bytes_read, client_address, client->port);
    }

    TCP_LOG(tid, cid, "Client (%s:%d) disconnected!", client_address, client->port);
}



int main(void)
{
    const char* error = NULL;

    TcpServer server = tcp_server(NULL, 6969, 0);
    if ((error = tcp_server_error(server))) {
        TCP_LOG(0, 0, "%s\n", error);
        return EXIT_FAILURE;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(server.host), ip_str, INET_ADDRSTRLEN);
    TCP_LOG(0, 0, "Serving at %s:%d", ip_str, server.port);

    while (true) {
        TCP_LOG(0, 0, "Waiting for client connection...%s", "");
        TcpClient client = tcp_accept(&server, handle_client);
        switch (tcp_client_status(client)) {
            case TCP_CLIENT_ERROR: {
                TCP_LOG(0, 0, "%s\n", tcp_client_error(client));
                break;
            } case TCP_CLIENT_REQUESTED_SHUTDOWN: {
                TCP_LOG(0, 0, "Shutting down the server!%s", "");
                tcp_close(&server);
                return EXIT_SUCCESS;
            } case TCP_CLIENT_CONNECTED: {
                char client_address[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client.host, client_address, INET_ADDRSTRLEN);

                TCP_LOG(0, 0, "Client %d connected at %s:%d", client.fd, client_address, client.port);
                break;
            }
        }
    }
}
