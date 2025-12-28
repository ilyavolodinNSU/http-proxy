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

// функция для обработки соединения в отдельном потоке
void* handle_connection(void* arg) {
    thread_data_t* data = (thread_data_t*)arg;
    int client_fd = data->client_fd;
    free(data);

    // буфер маленький!!!

    // чтение строки HTTP-запроса и заголовков из сокета клиента 
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        close(client_fd);
        return NULL;
    }
    buffer[bytes_read] = '\0';

    // извлекаю имя хоста из строки заголовков 
    char* host_start = strstr(buffer, "Host: ");
    if (!host_start) {
        const char* msg = "HTTP/1.0 400 Bad Request\r\n\r\nInvalid request";
        write(client_fd, msg, strlen(msg));
        close(client_fd);
        return NULL;
    }

    host_start += 6; // "Host: "
    char* host_end = strstr(host_start, "\r\n");
    if (!host_end) {
        close(client_fd);
        return NULL;
    }

    // извлекаю имя хоста и порт в host
    char host[256];
    int port = 80;
    int host_len = host_end - host_start;
    if (host_len > sizeof(host) - 1) {
        host_len = sizeof(host) - 1;
    }
    strncpy(host, host_start, host_len); // копирую до host_len символов из host_start в host
    host[host_len] = '\0';

    // чтобы прокси мог соединиться с конечным сервером ему нужно получить IP‑адрес + нужно создать сокет для соеденения с конечным сервером
    struct hostent* server = gethostbyname(host);
    if (!server) {
        // Ошибка 502 Bad Gateway: сервер, действуя как шлюз или прокси, получил недействительный ответ от вышестоящего сервера.
        const char* msg = "HTTP/1.0 502 Bad Gateway\r\n\r\nHost resolution failed";
        write(client_fd, msg, strlen(msg));
        close(client_fd);
        return NULL;
    }

    // создаю TCP сокет для соединения с конечным сервером
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        close(client_fd);
        return NULL;
    }

    // настройка адреса сервера server_addr
    struct sockaddr_in server_addr;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_addr.sin_family = AF_INET; // IPv4
    server_addr.sin_port = htons(port); // номер порта (в сетевом порядке байт) к которому будет привязан сокет

    // соединение с конечным сервером
    if (connect(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        close(client_fd);
        return NULL;
    }

    // пересылка запроса на конечный сервер
    write(server_fd, buffer, bytes_read);

    // пересылка ответа от конечного сервера клиенту
    // когда сервер начнёт присылать данные прокси, я читаю их порциями и шлю обратно в client_fd
    while (bytes_read = read(server_fd, buffer, sizeof(buffer))) {
        if (bytes_read < 0) {
            break;
        }
        
        if (write(client_fd, buffer, bytes_read) < 0) {
            break;
        }
    }

    close(server_fd);
    close(client_fd);
    return NULL;
}

int main() {
    // создаем TCP сокет
    /*
    socket() создает сокет, возвращает дескриптор сокета
        __domain - AF_INET - протокол IPv4
        __type - SOCK_STREAM - TCP
        __protocol - 0 - выбирается автоматически
    */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // SOL_SOCKET - уровень на котором работает опция SO_REUSEADDR. работаем с опциями уровня сокета (не конкретно для какого-то протокола)
    // SO_REUSEADDR позволяет переиспользовать порт сразу после завершения сервера. даже если он в состоянии TIME_WAIT
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        printf("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // настройка адреса сервера server_addr
    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_family = AF_INET; // IP адрес к которому будет привязан сокет (IPv4)
    server_addr.sin_port = htons(PORT); // номер порта 80 (в сетевом порядке байт) к которому будет привязан сокет

    // привязка сокета к адресу server_addr
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // начало прослушивания (5 потому что в мане прочитал что обычно <= 5)
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

        // создаю поток (раньше был fork()) для обработки нового соединения
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