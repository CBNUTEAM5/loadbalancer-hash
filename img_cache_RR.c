#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>

#define LISTENPORT 5294
#define PORTNUM3 5298
#define PORTNUM1 5297
#define PORTNUM2 5296
#define MAX_CLIENTS 100
#define NUM_SERVERS 3
#define QUEUE_SIZE 10 // Define the size of the queue
#define CACHE_TIMEOUT 30 // 캐시 만료 시간 (초)

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

typedef struct {
    char url[256];
    char response[1024];
    time_t timestamp;
} cache_entry;

server_info web_servers[] = {
    {"10.198.138.212", PORTNUM1}, // Example server IP, replace accordingly
    {"10.198.138.212", PORTNUM2}  // Example server IP, replace accordingly
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

cache_entry cache[100]; // 캐시 배열, 최대 100개의 항목 저장
int cache_count = 0; // 캐시 항목 개수

int current_server_index = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

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

// 캐시에서 요청 URL에 해당하는 응답 찾기
char* find_cache(const char* url) {
    for (int i = 0; i < cache_count; i++) {
        // 캐시된 응답이 유효한지 확인
        if (strcmp(cache[i].url, url) == 0 && (time(NULL) - cache[i].timestamp) < CACHE_TIMEOUT) {
            return cache[i].response;
        }
    }
    return NULL; // 캐시에서 찾을 수 없으면 NULL 반환
}

// 캐시에 응답 저장
void save_cache(const char* url, const char* response) {
    if (cache_count < 100) {
        strncpy(cache[cache_count].url, url, sizeof(cache[cache_count].url));
        strncpy(cache[cache_count].response, response, sizeof(cache[cache_count].response));
        cache[cache_count].timestamp = time(NULL);
        cache_count++;
    }
}

// 클라이언트 요청 처리
void* handle_client(void* arg) {
    while (1) {
        int client_socket = dequeue();

        // 클라이언트로부터 URL을 받은 후 캐시에서 확인
        char buffer[1024];
        int bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_received <= 0) {
            perror("recv from client failed");
            close(client_socket);
            continue;
        }

        buffer[bytes_received] = '\0'; // URL이 들어있는 버퍼 종료 처리

        // 캐시에서 해당 URL의 응답을 찾기
        char* cached_response = find_cache(buffer);
        if (cached_response) {
            // 캐시에서 찾은 응답을 클라이언트로 전송
            send(client_socket, cached_response, strlen(cached_response), 0);
            close(client_socket);
            continue;
        }

        // 라운드 로빈으로 서버 선택
        int server_index = load_balance();
        server_info selected_server = web_servers[server_index];

        // 서버와의 연결 설정
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

        // 서버에 연결
        if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Server connect failed");
            close(client_socket);
            close(server_socket);
            continue;
        }

        // 클라이언트 요청을 서버로 전달
        send(server_socket, buffer, bytes_received, 0);

        // 서버 응답을 클라이언트로 전달하고 캐시 저장
        while ((bytes_received = recv(server_socket, buffer, sizeof(buffer), 0)) > 0) {
            send(client_socket, buffer, bytes_received, 0);
        }

        if (bytes_received < 0) {
            perror("recv from server failed");
        }

        // 서버 응답을 캐시에 저장
        save_cache(buffer, buffer);

        // 소켓 닫기
        close(client_socket);
        close(server_socket);
    }
    return NULL;
}

// 메인 함수
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

    // 바인드 및 리슨
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

    // 워커 스레드 생성
    pthread_t workers[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&workers[i], NULL, handle_client, NULL);
    }

    // 클라이언트 연결 수락 및 큐에 추가
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
