캐시

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdint.h>

#define LISTENPORT 5294
#define PORTNUM1 5297
#define PORTNUM2 5296
#define PORTNUM3 5298
#define MAX_CLIENTS 100
#define NUM_SERVERS 3
#define QUEUE_SIZE 10
#define CACHE_SIZE 5 // 캐시의 크기

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

// LRU 캐시 노드 구조체
typedef struct CacheNode {
    char key[1024];              // 요청 키
    char value[1024];            // 응답 값
    struct CacheNode* prev;      // 이전 노드
    struct CacheNode* next;      // 다음 노드
} CacheNode;

typedef struct {
    CacheNode* head;             // 캐시의 시작 노드
    CacheNode* tail;             // 캐시의 끝 노드
    int size;                    // 현재 캐시 크기
    pthread_mutex_t mutex;       // 캐시 접근 동기화
} LRUCache;

server_info web_servers[] = {
    {"10.198.138.212", PORTNUM1},
    {"10.198.138.212", PORTNUM2},
    {"10.198.138.212", PORTNUM3}
};

request_queue queue = {
    .front = 0,
    .rear = 0,
    .count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond_non_empty = PTHREAD_COND_INITIALIZER,
    .cond_non_full = PTHREAD_COND_INITIALIZER
};

int current_server_index = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// LRU 캐시 초기화
LRUCache cache = {
    .head = NULL,
    .tail = NULL,
    .size = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

// 라운드 로빈 방식으로 서버 선택
int load_balance() {
    pthread_mutex_lock(&lock);
    int server_index = current_server_index;
    current_server_index = (current_server_index + 1) % NUM_SERVERS;
    pthread_mutex_unlock(&lock);
    return server_index;
}

// 클라이언트 요청을 큐에 추가
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

// 클라이언트 요청을 큐에서 가져오기
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

// 캐시에서 키 검색
CacheNode* cache_search(const char* key) {
    pthread_mutex_lock(&cache.mutex);
    CacheNode* node = cache.head;
    while (node) {
        if (strcmp(node->key, key) == 0) {
            // 캐시 히트 시, 노드를 맨 앞으로 이동
            if (node != cache.head) {
                // 노드 제거
                if (node->prev) node->prev->next = node->next;
                if (node->next) node->next->prev = node->prev;
                if (node == cache.tail) cache.tail = node->prev;

                // 노드를 맨 앞으로 이동
                node->next = cache.head;
                node->prev = NULL;
                if (cache.head) cache.head->prev = node;
                cache.head = node;
            }
            pthread_mutex_unlock(&cache.mutex);
            return node;
        }
        node = node->next;
    }
    pthread_mutex_unlock(&cache.mutex);
    return NULL;
}

// 캐시에 새로운 노드 추가
void cache_add(const char* key, const char* value) {
    pthread_mutex_lock(&cache.mutex);
    if (cache.size >= CACHE_SIZE) {
        // 캐시가 꽉 찼으면 가장 오래된 노드 제거
        CacheNode* old_tail = cache.tail;
        if (old_tail->prev) old_tail->prev->next = NULL;
        cache.tail = old_tail->prev;
        free(old_tail);
        cache.size--;
    }

    // 새로운 노드 추가
    CacheNode* new_node = (CacheNode*)malloc(sizeof(CacheNode));
    strcpy(new_node->key, key);
    strcpy(new_node->value, value);
    new_node->next = cache.head;
    new_node->prev = NULL;
    if (cache.head) cache.head->prev = new_node;
    cache.head = new_node;
    if (!cache.tail) cache.tail = new_node;
    cache.size++;
    pthread_mutex_unlock(&cache.mutex);
}

// 클라이언트 요청 처리
void* handle_client(void* arg) {
    while (1) {
        int client_socket = dequeue();

        // 클라이언트 요청 수신
        char buffer[1024];
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0) {
            perror("recv from client failed");
            close(client_socket);
            continue;
        }
        buffer[bytes_received] = '\0';

        // 캐시 확인
        CacheNode* cached_response = cache_search(buffer);
        if (cached_response) {
            // 캐시 응답 반환
            send(client_socket, cached_response->value, strlen(cached_response->value), 0);
            close(client_socket);
            continue;
        }

        // 서버 선택 및 연결
        int server_index = load_balance();
        server_info selected_server = web_servers[server_index];
        int server_socket;
        struct sockaddr_in server_addr;
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket < 0) {
            perror("Socket creation failed for server");
            close(client_socket);
            continue;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(selected_server.port);
        inet_pton(AF_INET, selected_server.ip, &server_addr.sin_addr);

        if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Server connect failed");
            close(client_socket);
            close(server_socket);
            continue;
        }

        // 서버로 요청 전달
        send(server_socket, buffer, bytes_received, 0);

        // 서버 응답 수신
        bytes_received = recv(server_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';
            send(client_socket, buffer, bytes_received, 0);

            // 응답을 캐시에 저장
            cache_add(buffer, buffer);
        }
        else {
            perror("recv from server failed");
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
        perror("Bind failed");
        close(server_socket);
        return -1;
    }

    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        close(server_socket);
        return -1;
    }

    pthread_t workers[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i], NULL, handle_client, NULL);
    }

    while (1) {
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }
        enqueue(client_socket);
    }

    close(server_socket);
    return 0;
}
