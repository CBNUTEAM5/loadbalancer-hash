#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define LISTENPORT 5294
#define PORTNUM3 5298
#define PORTNUM1 5297
#define PORTNUM2 5296
#define MAX_CLIENTS 100
#define NUM_SERVERS 3

typedef struct {
    char ip[16];
    int port;
} server_info;

server_info web_servers[] = {
    {"10.198.138.212", PORTNUM1},
    {"10.198.138.212", PORTNUM2},
    {"10.198.138.212", PORTNUM3}
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

void* handle_client(void* arg) {
    int client_socket = *(int*)arg; 
    free(arg); 

    // 클라이언트 IP
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

    // 해시 로드밸런싱
    int server_index = load_balance(client_ip);
    server_info selected_server = web_servers[server_index];

    // 서버와의 연결
    int server_socket;
    struct sockaddr_in server_addr;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed for server");
        close(client_socket);
        return NULL;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(selected_server.port);
    inet_pton(AF_INET, selected_server.ip, &server_addr.sin_addr);

    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Server connect failed");
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
        perror("recv from client failed");
        close(client_socket);
        close(server_socket);
        return NULL;
    }

    // 서버 -> 클라
    while ((bytes_received = recv(server_socket, buffer, sizeof(buffer), 0)) > 0) {
        send(client_socket, buffer, bytes_received, 0);
    }

    if (bytes_received < 0) {
        perror("recv from server failed");
    }

    close(client_socket);
    close(server_socket);

    return NULL;
}

int main() {
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // 서버 소켓 생성
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(LISTENPORT);

    
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        return -1;
    }

    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        close(server_socket);
        return -1;
    }

    printf("Server listening on port %d\n", LISTENPORT);

    // 스레드 생성
    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }

        int* client_socket_ptr = malloc(sizeof(int));
        if (!client_socket_ptr) {
            perror("Memory allocation failed");
            close(client_socket);
            continue;
        }

        *client_socket_ptr = client_socket;

        pthread_t thread_id;
        pthread_create(&thread_id, NULL, handle_client, client_socket_ptr);
        pthread_detach(thread_id); 
    }

    close(server_socket);
    return 0;
}
