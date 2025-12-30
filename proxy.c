#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT 80
#define BUFFER_SIZE 4096

// для передачи параметров в поток
typedef struct {
    int client_fd;
} thread_data_t;

ssize_t read_request_headers(int fd, char *buf, size_t bufsize);
int extract_host_from_headers(const char *headers, char *host, size_t hostsize);
int resolve_and_connect(const char *host, int port);
ssize_t forward_request(int server_fd, const char *buf, size_t len);
void relay_response(int server_fd, int client_fd);

void* handle_connection(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    int client_fd = data->client_fd;
    free(data);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read_request_headers(client_fd, buffer, sizeof(buffer));
    if (bytes_read <= 0) {
        close(client_fd);
        return NULL;
    }

    char host[256];
    int port = 80; // оставляем фиксированный порт, как в оригинале
    if (extract_host_from_headers(buffer, host, sizeof(host)) != 0) {
        const char* msg = "HTTP/1.0 400 Bad Request\r\n\r\nInvalid request";
        write(client_fd, msg, strlen(msg));
        close(client_fd);
        return NULL;
    }

    int server_fd = resolve_and_connect(host, port);
    if (server_fd < 0) {
        const char* msg = "HTTP/1.0 502 Bad Gateway\r\n\r\nHost resolution failed";
        write(client_fd, msg, strlen(msg));
        close(client_fd);
        return NULL;
    }

    if (forward_request(server_fd, buffer, (size_t)bytes_read) < 0) {
        close(server_fd);
        close(client_fd);
        return NULL;
    }

    relay_response(server_fd, client_fd);

    close(server_fd);
    close(client_fd);
    return NULL;
}

// читает заголовки (до "\r\n\r\n") в буфер; возвращает количество байт или <=0 при ошибке
ssize_t read_request_headers(int fd, char *buf, size_t bufsize) {
    if (!buf || bufsize == 0) return -1;
    ssize_t total = 0;
    ssize_t n;

    // читаем в цикл, пока не получим конец заголовков "\r\n\r\n" или не заполнится буфер
    while ((n = read(fd, buf + total, bufsize - 1 - total)) > 0) {
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL) {
            break; // получили все заголовки
        }
        if (total >= (ssize_t)(bufsize - 1)) {
            break; // буфер заполнен
        }
    }

    if (n < 0 && total == 0) return -1;
    return total;
}

// извлекает значение host: из заголовков, записывает в host (null-terminated)
// возвращает 0 при успехе, -1 при ошибке
int extract_host_from_headers(const char *headers, char *host, size_t hostsize) {
    if (!headers || !host || hostsize == 0) return -1;
    const char *h = strstr(headers, "Host: ");
    if (!h) return -1;
    h += 6; // пропускаем "Host: "
    const char *h_end = strstr(h, "\r\n");
    if (!h_end) return -1;
    size_t len = (size_t)(h_end - h);
    if (len >= hostsize) len = hostsize - 1;
    // копируем и удаляем ведущие/хвостовые пробелы
    size_t i = 0;
    // пропускаем ведущие пробелы
    while (i < len && (h[i] == ' ' || h[i] == '\t')) i++;
    size_t start = i;
    // находим реальную длину
    size_t actual_len = len - start;
    while (actual_len > 0 && (h[start + actual_len - 1] == ' ' || h[start + actual_len - 1] == '\t')) actual_len--;
    if (actual_len >= hostsize) actual_len = hostsize - 1;
    memcpy(host, h + start, actual_len);
    host[actual_len] = '\0';
    return 0;
}

// разрешает имя host и устанавливает соединение; возвращает fd сокета или -1 при ошибке
int resolve_and_connect(const char *host, int port) {
    struct hostent* server = gethostbyname(host);
    if (!server) return -1;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return -1;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (connect(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return -1;
    }

    return server_fd;
}

// форвардит уже прочитанные данные запроса на server_fd
ssize_t forward_request(int server_fd, const char *buf, size_t len) {
    if (!buf || len == 0) return 0;
    ssize_t written = 0;
    while ((size_t)written < len) {
        ssize_t n = write(server_fd, buf + written, len - (size_t)written);
        if (n < 0) return -1;
        written += n;
    }
    return written;
}

// пересылает ответ от server_fd в client_fd
void relay_response(int server_fd, int client_fd) {
    char buffer[BUFFER_SIZE];
    ssize_t n;
    while ((n = read(server_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(client_fd, buffer + sent, n - sent);
            if (w <= 0) return;
            sent += w;
        }
    }
}

int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        printf("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) < 0) {
        printf("listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("прокси сервер слушает порт %d...\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            printf("accept failed");
            continue;
        }

        printf("новое подключение от %s:%d\n", inet_ntoa(client_addr.sin_addr), client_addr.sin_port);

        pthread_t tid;
        thread_data_t* data = malloc(sizeof(thread_data_t));
        data->client_fd = client_fd;

        if (pthread_create(&tid, NULL, handle_connection, data)) {
            printf("pthread_create failed\n");
            close(client_fd);
            free(data);
        } else {
            pthread_detach(tid);
        }
    }

    close(server_fd);
    return 0;
}
