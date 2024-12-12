#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <regex.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include "../include/ftp_client.h"

#define SERVER_PORT 21
#define MAX_BUFFER_SIZE 1024
#define MAX_LENGTH 1024
#define FTP_PORT 21
#define AT              "@"
#define BAR             "/"
#define HOST_REGEX      "%*[^/]//%[^/]"
#define HOST_AT_REGEX   "%*[^/]//%*[^@]@%[^/]"
#define RESOURCE_REGEX  "%*[^/]//%*[^/]/%s"
#define USER_REGEX      "%*[^/]//%[^:/]"
#define PASS_REGEX      "%*[^/]//%*[^:]:%[^@\n$]"
#define RESPCODE_REGEX  "%d"
#define PASSIVE_REGEX   "%*[^(](%d,%d,%d,%d,%d,%d)%*[^\n$)]"

/* Default login for case 'ftp://<host>/<url-path>' */
#define DEFAULT_USER        "anonymous"
#define DEFAULT_PASSWORD    "password"

typedef enum { START, SINGLE, MULTIPLE, END } ResponseState;

int parse_url(const char *url_str, URL *parsed_url) { // Renomeado para url_str
    regex_t regex;
    regcomp(&regex, BAR, 0);
    if (regexec(&regex, url_str, 0, NULL, 0)) return -1; // Usar url_str

    regcomp(&regex, AT, 0);
    if (regexec(&regex, url_str, 0, NULL, 0) != 0) { // ftp://<host>/<url-path>
        sscanf(url_str, HOST_REGEX, parsed_url->host); // Usar parsed_url
        strcpy(parsed_url->user, DEFAULT_USER);
        strcpy(parsed_url->password, DEFAULT_PASSWORD);
    } else { // ftp://[<user>:<password>@]<host>/<url-path>
        sscanf(url_str, HOST_AT_REGEX, parsed_url->host);
        sscanf(url_str, USER_REGEX, parsed_url->user);
        sscanf(url_str, PASS_REGEX, parsed_url->password);
    }

    sscanf(url_str, RESOURCE_REGEX, parsed_url->path);
    strcpy(parsed_url->filename, strrchr(url_str, '/') + 1);

    struct hostent *h;
    if (strlen(parsed_url->host) == 0) return -1;
    if ((h = gethostbyname(parsed_url->host)) == NULL) {
        printf("Invalid hostname '%s'\n", parsed_url->host);
        exit(-1);
    }
    strcpy(parsed_url->ip, inet_ntoa(*((struct in_addr *) h->h_addr)));

    return !(strlen(parsed_url->host) && strlen(parsed_url->user) && 
           strlen(parsed_url->password) && strlen(parsed_url->path) && strlen(parsed_url->filename));
}

int createSocket(char *ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);  
    server_addr.sin_port = htons(port); 
    
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        exit(-1);
    }
    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        exit(-1);
    }
    
    return sockfd;
}

int readResponse(int socket_fd, char *response_buffer) {
    fd_set socket_set;
    struct timeval timeout;
    int bytes_received = 0, total_bytes = 0, response_code = 0;

    memset(response_buffer, 0, MAX_BUFFER_SIZE);

    while (1) {
        FD_ZERO(&socket_set);
        FD_SET(socket_fd, &socket_set);

        timeout.tv_sec = 1;  
        timeout.tv_usec = 0;

        int activity = select(socket_fd + 1, &socket_set, NULL, NULL, &timeout);
        if (activity == -1) {
            perror("Select error");
            return -1;
        } else if (activity == 0) {
            fprintf(stderr, "Timeout: No data received from server\n");
            break; // Timeout
        } else {
            bytes_received = recv(socket_fd, response_buffer + total_bytes, 
                                  MAX_BUFFER_SIZE - total_bytes - 1, 0);
            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    fprintf(stderr, "Connection closed by server.\n");
                } else {
                    perror("Error receiving data");
                }
                break;
            }
            total_bytes += bytes_received;

            // Debug: Mostrar o trecho recebido
            printf("Partial Server Response: '%.*s'\n", bytes_received, 
                   response_buffer + total_bytes - bytes_received);

            // Verificar se temos o final da resposta
            if (strstr(response_buffer, "\r\n")) {
                // Respostas de múltiplas linhas do FTP começam com "xyz-" e terminam com "xyz " (xyz = código)
                char *last_line = strrchr(response_buffer, '\n'); // Encontrar a última nova linha
                if (last_line && strlen(last_line) > 4 && isdigit(last_line[1]) && 
                    isdigit(last_line[2]) && isdigit(last_line[3]) && last_line[4] == ' ') {
                    break; // Marcador de final de resposta válido
                }
            }
        }
    }

    response_buffer[total_bytes] = '\0'; // Null-terminar a string de resposta
    printf("Full Server Response: '%s'\n", response_buffer);

    if (total_bytes == 0) {
        fprintf(stderr, "Error: No data received from the server.\n");
        return -1;
    }

    // Analisar o código de resposta da primeira linha
    if (sscanf(response_buffer, "%3d", &response_code) != 1) {
        fprintf(stderr, "Failed to parse server response: '%s'\n", response_buffer);
        return -1;
    }

    return response_code;
}
int authConn(const int socket, const char* user, const char* pass) {
    char userCommand[MAX_LENGTH]; 
    char passCommand[MAX_LENGTH]; 
    char answer[MAX_LENGTH];
    
    // Verificar se user e pass não são NULL
    if (user == NULL || pass == NULL) {
        printf("Error: Username or password is NULL\n");
        return -1;
    }

    // Construir o comando USER com verificação de tamanho
    if (snprintf(userCommand, sizeof(userCommand), "USER %s\r\n", user) < 0) {
        printf("Error creating USER command\n");
        return -1;
    }
    
    // Debug: mostrar o comando que será enviado
    printf("Sending USER command: %s", userCommand);
    
    // Enviar comando USER
    if (write(socket, userCommand, strlen(userCommand)) < 0) {
        perror("Error sending USER command");
        return -1;
    }

    int responseCode = readResponse(socket, answer);
    printf("Response after USER command: %s\n", answer);
    if (responseCode != 331) { // 331 User name okay, need password.
        printf("Authentication failed: %s\n", answer);
        return -1;
    }

    // Construir o comando PASS com verificação de tamanho
    if (snprintf(passCommand, sizeof(passCommand), "PASS %s\r\n", pass) < 0) {
        printf("Error creating PASS command\n");
        return -1;
    }

    // Enviar comando PASS
    if (write(socket, passCommand, strlen(passCommand)) < 0) {
        perror("Error sending PASS command");
        return -1;
    }

    responseCode = readResponse(socket, answer);
    printf("Response after PASS command: %s\n", answer);
    return responseCode;
}

int passiveMode(const int socket, char *ip, int *port) {
    char answer[MAX_LENGTH];
    int ip1, ip2, ip3, ip4, port1, port2;
    write(socket, "PASV\r\n", 6);
    if (readResponse(socket, answer) != 227) return -1; // 227 Entering Passive Mode.

    sscanf(answer, PASSIVE_REGEX, &ip1, &ip2, &ip3, &ip4, &port1, &port2);
    *port = port1 * 256 + port2;
    sprintf(ip, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);

    return 227;
}

int requestResource(const int socket, char *resource) {
    char typeCommand[] = "TYPE I\r\n";
    char fileCommand[MAX_BUFFER_SIZE], answer[MAX_LENGTH];
    
    // Primeiro, definir modo de transferência binário
    write(socket, typeCommand, strlen(typeCommand));
    int response = readResponse(socket, answer);
    if (response != 200) {  // 200 Type set to I
        printf("Failed to set binary mode: %s\n", answer);
        return -1;
    }

    printf("Resource: %s\n", resource);
    printf("Socket: %d\n", socket);

    snprintf(fileCommand, sizeof(fileCommand), "RETR %s\r\n", resource);
    if (write(socket, fileCommand, strlen(fileCommand)) < 0) {
        perror("Error sending RETR command");
        return -1;
    }
    printf("File command: %s", fileCommand);

    response = readResponse(socket, answer);
    printf("Response after RETR command: %d\n", response);
    printf("Server response: %s\n", answer);
    return response;
}
 


int getResource(const int socketA, const int socketB, char *filename) {
    printf("Saving file to: %s\n", filename);

    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        perror("File creation failed");
        return -1;
    }

    char buffer[MAX_BUFFER_SIZE];
    int bytes_read;
    size_t total_bytes = 0;
    time_t last_update = time(NULL);

    while ((bytes_read = read(socketB, buffer, sizeof(buffer))) > 0) {
        if (fwrite(buffer, 1, bytes_read, file) < bytes_read) {
            perror("File writing failed");
            fclose(file);
            return -1;
        }
        
        total_bytes += bytes_read;
        
        // Atualizar progresso a cada segundo
        time_t current_time = time(NULL);
        if (current_time > last_update) {
            printf("\rReceived: %.2f MB", total_bytes / (1024.0 * 1024.0));
            fflush(stdout);
            last_update = current_time;
        }
    }

    printf("\nFile transfer complete. Total received: %.2f MB\n", 
           total_bytes / (1024.0 * 1024.0));
    
    fclose(file);

    // Fechar o socket de dados após a transferência
    close(socketB);

    // Aguardar a resposta de conclusão do servidor no socket de controle
    char answer[MAX_LENGTH];
    int response = readResponse(socketA, answer);
    printf("Transfer completion response: %d\n", response);
    
    return response;
}

int closeConnection(const int socketA, const int socketB) {
    char answer[MAX_LENGTH];
    int result = 0;
    
    // Enviar comando QUIT
    write(socketA, "QUIT\r\n", 6);
    if(readResponse(socketA, answer) != 221) return -1; 
    
    // Fechar o socket de controle
    if (close(socketA) < 0) {
        perror("Error closing control socket");
        result = -1;
    }
    
    // Tentar fechar o socket de dados apenas se ainda não foi fechado
    if (fcntl(socketB, F_GETFD) != -1) {
        if (close(socketB) < 0) {
            perror("Error closing data socket");
            result = -1;
        }
    }
    
    return result;
}
int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: ./download ftp://[<user>:<password>@]<host>/<url-path>\n");
        exit(-1);
    } 

    URL url;
    memset(&url, 0, sizeof(url));
    if (parse_url(argv[1], &url) != 0) { 
        printf("Parse error. Usage: ./download ftp://[<user>:<password>@]<host>/<url-path>\n");
        exit(-1);
    }
    
    printf("Host: %s\nResource: %s\nFile: %s\nUser: %s\nPassword: %s\nIP Address: %s\n", url.host, url.path, url.filename, url.user, url.password, url.ip);

    char answer[MAX_LENGTH];
    printf("Connecting to %s:%d\n", url.ip, FTP_PORT);
    int socketA = createSocket(url.ip, FTP_PORT);
    printf("Socket to '%s' and port %d created\n", url.ip, FTP_PORT);
    int responseCode = readResponse(socketA, answer);
    printf("Response from readResponse: %d\n", responseCode);
    if (socketA < 0 || responseCode != 220) { // 220 Service ready for new user.
        printf("Socket to '%s' and port %d failed\n", url.ip, FTP_PORT);
        exit(-1);
    }
    printf("Connected to %s:%d\n", url.ip, FTP_PORT);
    
    if (authConn(socketA, url.user, url.password) != 230) { // 230 User logged in, proceed.
        printf("Authentication failed with username = '%s' and password = '%s'.\n", url.user, url.password);
        exit(-1);
    }
    printf("Authenticated with username = '%s' and password = '%s'\n", url.user, url.password);
    int port;
    char ip[MAX_LENGTH];
    if (passiveMode(socketA, ip, &port) != 227) {
        printf("Passive mode failed\n");
        printf("Response from PASV command: %s\n", answer); // Adicione esta linha
        exit(-1);
    }
    printf("Passive mode established. IP: %s, Port: %d\n", ip, port);


    int socketB = createSocket(ip, port);
    if (socketB < 0) {
        printf("Socket to '%s:%d' failed\n", ip, port);
        exit(-1);
    } else {
        printf("Data connection established to %s:%d\n", ip, port);
    }

    int response = requestResource(socketA, url.path);
    printf("Response from requestResource: %d\n", response);
    if (response != 150  && response !=125) {
        printf("Unknown resource '%s' in '%s:%d'\n", url.path, ip, port);
        exit(-1);
    }
    int transferResponse = getResource(socketA, socketB, url.filename);
    printf("Response from getResource: %d\n", transferResponse);
    if (transferResponse != 226) {
        printf("Error transferring file '%s' from '%s:%d'\n", url.filename, ip, port);
        printf("Transfer response: %s\n", answer); 
        exit(-1);
    }

    if (closeConnection(socketA, socketB) != 0) {
        printf("Sockets close error\n");
        exit(-1);
    }

    printf("File downloaded successfully.\n");
    return 0;
}