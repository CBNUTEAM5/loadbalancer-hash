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
#define PORTNUM1 5295
#define PORTNUM2 5296
#define MAX_CLIENTS 100
#define NUM_SERVERS 2
#define QUEUE_SIZE 10 // Define the size of the queue

typedef struct {
    char ip[16];
    int port;
} server_info;

typedef struct {
    int client_socket;
} client_request;

typedef struct {
    client_request requests[QUEUE_SIZE];
    int front, rear, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_non_empty;
    pthread_cond_t cond_non_full;
} request_queue;

server_info web_servers[] = {
    {"10.198.138.212", PORTNUM1},
    {"10.198.138.212",PORTNUM2}  //Example server IP, replace accordingly
};

request_queue queue = {
    .front = 0,
    .rear = 0,
    .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond_non_empty = PTHREAD_COND_INITIALIZER,
    .cond_non_full = PTHREAD_COND_INITIALIZER
};


int current_server_index = 0;  // 현재 선택된 서버의 인덱스
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // 동기화를 위한 mutex

// 라운드 로빈 방식으로 서버 선택
int load_balance(char* client_ip) {
    pthread_mutex_lock(&lock);                     // 임계 구역 시작: mutex 잠금
    int server_index = current_server_index;       // 현재 서버 인덱스를 저장
    current_server_index = (current_server_index + 1) % NUM_SERVERS; // 다음 서버로 이동
    pthread_mutex_unlock(&lock);                   // 임계 구역 종료: mutex 해제
    return server_index;                           // 선택된 서버 인덱스 반환
}


void enqueue(int client_socket) {
    pthread_mutex_lock(&queue.mutex);
    while (queue.count == QUEUE_SIZE) {
        pthread_cond_wait(&queue.cond_non_full, &queue.mutex);
    }
    queue.requests[queue.rear].client_socket = client_socket;
    queue.rear = (queue.rear + 1) % QUEUE_SIZE;
    queue.count++;
    pthread_cond_signal(&queue.cond_non_empty);
    pthread_mutex_unlock(&queue.mutex);
}

int dequeue() {
    pthread_mutex_lock(&queue.mutex);
    while (queue.count == 0) {
        pthread_cond_wait(&queue.cond_non_empty, &queue.mutex);
    }
    int client_socket = queue.requests[queue.front].client_socket;
    queue.front = (queue.front + 1) % QUEUE_SIZE;
    queue.count--;
    pthread_cond_signal(&queue.cond_non_full);
    pthread_mutex_unlock(&queue.mutex);
    return client_socket;
}

void* handle_client(void* arg) {
    while (1) {
        int client_socket = dequeue();

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
            continue;
        }
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(selected_server.port);
        inet_pton(AF_INET, selected_server.ip, &server_addr.sin_addr);

        if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("connect");
            close(client_socket);
            close(server_socket);
            continue;
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
            continue;
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
    }
    return NULL;
}

int main() {
    int server_socket;
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

    pthread_t workers[4]; 
    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i], NULL, handle_client, NULL);
    }

    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        enqueue(client_socket);
    }

    close(server_socket);
    return 0;
}

