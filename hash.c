#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define LISTENPORT 8080
#define PORTNUM 9100 
#define MAX_CLIENTS 100
#define NUM_SERVERS 1

typedef struct {
    char ip[16]; 
    int port;   
} server_info;

server_info web_servers[] = {
    {"", PORTNUM},
    {"", PORTNUM},
    {"", PORTNUM}
};
unsigned int murmur_hash(char* key) {
    unsigned int seed = 0x1234abcd;
    unsigned int m = 0x5bd1e995;
    unsigned int r = 24;
    unsigned int len = strlen(key);
    unsigned int h = seed ^ len;
    const unsigned char* data = (const unsigned char*)key;
    while (len >= 4) {
        unsigned int k = *(unsigned int*)data;
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        data += 4;
        len -= 4;
    }
    switch (len) {
    case 3: h ^= data[2] << 16;
    case 2: h ^= data[1] << 8;
    case 1: h ^= data[0];
        h *= m;
    };
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h % NUM_SERVERS;
}

int load_balance(char* client_ip) {
    return murmur_hash(client_ip);
}

void* handle_client(void* client_sock) {
    int client_socket = *(int*)client_sock;
    free(client_sock);

    char client_ip[16];
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(client_socket, (struct sockaddr*)&addr, &addr_len) == 0) {
        strcpy(client_ip, inet_ntoa(addr.sin_addr));
    }
    else {
        strcpy(client_ip, "Unknown");
    }

    int server_index = load_balance(client_ip);
    server_info selected_server = web_servers[server_index];

    int server_socket;
    struct sockaddr_in server_addr;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed for server");
        close(client_socket);
        return NULL;
    }
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(selected_server.port);
    inet_pton(AF_INET, selected_server.ip, &server_addr.sin_addr);

    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(client_socket);
        close(server_socket);
        return NULL;
    }

    char buffer[1024];
    int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
    if (bytes_received > 0) {
        send(server_socket, buffer, bytes_received, 0);
    }
    else if (bytes_received < 0) {
        perror("recv");
        close(client_socket);
        close(server_socket);
        return NULL;
    }

    bytes_received = recv(server_socket, buffer, sizeof(buffer), 0);
    if (bytes_received > 0) {
        send(client_socket, buffer, bytes_received, 0);
    }
    else if (bytes_received < 0) {
        perror("recv");
    }

    close(client_socket);
    close(server_socket);
    return NULL;
}

int main() {
    int server_socket, * new_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);


    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(LISTENPORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return -1;
    }

    listen(server_socket, MAX_CLIENTS);

    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        
        pthread_t tid;
        new_sock = malloc(sizeof(int));
        *new_sock = client_socket;
        if (pthread_create(&tid, NULL, handle_client, (void*)new_sock) != 0) {
            close(client_socket);
            free(new_sock);
        }
    }

    close(server_socket);
    return 0;
}
