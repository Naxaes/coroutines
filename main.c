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

    int id = coroutine_id();

    char client_address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client->host), client_address, INET_ADDRSTRLEN);
    printf("[%d]: Client connected at %s:%d.\n", id, client_address, client->port);


    while (true) {
        printf("[%d]: Waiting reading data from client!\n", id);
        ssize_t bytes_read = tcp_read(client, read_buffer, sizeof(read_buffer));
        if (bytes_read == 0) {
            printf("[%d]: Client disconnected.\n", id);
            return;
        } else if (bytes_read < 0) {
            printf("[%d]: Error reading data from client. Exiting...\n", id);
            perror("handle_client");
            return;
        }
        printf("[%d]: Read %zd bytes from client\n", id, bytes_read);
        printf("'%.*s'\n", (int)bytes_read, read_buffer);

        if (strncmp(read_buffer, "exit", sizeof("exit")-1) == 0) {
            break;
        } else if (strncmp(read_buffer, "shutdown", sizeof("shutdown")-1) == 0) {
            tcp_request_shutdown(*client);
            break;
        }

        String index = load_html_file("resources/index.html");
        if (index.data == NULL) {
            printf("[%d]: Error loading index.html\n", id);
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

        printf("[%d]: Waiting writing data to client!\n", id);
        ssize_t bytes_written = tcp_write(client, write_buffer, header_len+index.size+bytes_read);
        free(index.data);
        if (bytes_written <= 0) {
            printf("[%d]: Couldn't write anything to client. Exiting...\n", id);
            return;
        }
        printf("[%d]: Wrote %lu bytes to client\n", id, index.size+bytes_read);
    }

    printf("[%d]: Client disconnected!\n", id);
}


int main()
{
    const char* error = NULL;

    TcpServer server = tcp_server( .port=6969 );
    if ((error = tcp_server_error(server))) {
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(server.host), ip_str, INET_ADDRSTRLEN);
    printf("Serving at %s:%d\n", ip_str, server.port);

    while (true) {
        printf("[%d]: Waiting for client connection...\n", server.fd);
        TcpClient client = tcp_accept(&server, handle_client);
        if ((error = tcp_client_error(client))) {
            fprintf(stderr, "%s\n", error);
        } else {
            printf("[%d]: Client %d connected!\n", server.fd, client.fd);

            char client_address[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client.host), client_address, INET_ADDRSTRLEN);

            printf("[%d]: Client %d connected at %s:%d.\n", server.fd, client.fd, client_address, client.port);
        }

        if (tcp_shutdown_requested()) {
            printf("[%d]: Shutting down the server!\n", server.fd);
            tcp_close(&server);
            return EXIT_SUCCESS;
        }
    }
}
