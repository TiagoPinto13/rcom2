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
#include "../include/download.h"

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

int readResponse(const int socket, char* buffer) {
    printf("Reading response from socket %d\n", socket);
    char byte;
    int index = 0, responseCode;
    ResponseState state = START;

    memset(buffer, 0, MAX_LENGTH);
    printf("Buffer: %s\n", buffer);

    while (state != END) {
        ssize_t bytesRead = read(socket, &byte, 1);
        if (bytesRead < 0) {
            perror("Error reading from socket");
            return -1; // Erro na leitura
        } else if (bytesRead == 0) {
            // Se read retornar 0, a conexão foi fechada
            printf("Connection closed by the server\n");
            return -1;
        }

        switch (state) {
            case START:
                printf("Start: %c\n", byte);
                if (byte == ' ') state = SINGLE;
                else if (byte == '-') state = MULTIPLE;
                else if (byte == '\n') state = END;
                else buffer[index++] = byte;
                break;
            case SINGLE:
                printf("Single line: %c\n", byte);
                if (byte == '\n') state = END;
                else buffer[index++] = byte;
                break;
            case MULTIPLE:
                printf("Multiple lines: %c\n", byte);

                if (byte == '\n') {
                    memset(buffer, 0, MAX_LENGTH); // Limpa o buffer
                    state = START;
                    index = 0;
                } else buffer[index++] = byte;
                break;
            case END:
                break;
            default:
                break;
        }
    }

    sscanf(buffer, RESPCODE_REGEX, &responseCode);
    return responseCode;
}
int authConn(const int socket, const char* user, const char* pass) {
    char userCommand[5 + strlen(user) + 1]; 
    sprintf(userCommand, "USER %s\r\n", user);
    char passCommand[5 + strlen(pass) + 1]; 
    sprintf(passCommand, "PASS %s\r\n", pass);
    char answer[MAX_LENGTH];
    
    write(socket, userCommand, strlen(userCommand));
    printf("User command: %s\n", userCommand); // Adicione esta linha
    int responseCode = readResponse(socket, answer);
    printf("Response after USER command: %s", answer); // Adicione esta linha
    printf("Response after USER command: %s", answer); // Adicione esta linha
    if (responseCode != 331) { // 331 User name okay, need password.
        printf("Authentication failed: %s\n", answer); // Imprime a resposta do servidor
        exit(-1);
    }

    write(socket, passCommand, strlen(passCommand));
    responseCode = readResponse(socket, answer);
    printf("Response after PASS command: %s", answer); // Adicione esta linha
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
    char fileCommand[5 + strlen(resource) + 1], answer[MAX_LENGTH];
    printf("Resource: %s\n", resource);
    printf("Socket: %d\n", socket);

    sprintf(fileCommand, "RETR %s\r\n", resource);
    write(socket, fileCommand, strlen(fileCommand));
    printf("File command: %s\n", fileCommand); // Adicione esta linha

    // Ler a resposta do servidor
    int response = readResponse(socket, answer);
    printf("Response after RETR command: %d\n", response); // Adicione esta linha
    printf("Server response: %s\n", answer); // Adicione esta linha para imprimir a resposta do servidor
    return response;
}

int getResource(const int socketA, const int socketB, char *filename) {
    FILE *fd = fopen(filename, "wb");
    if (fd == NULL) {
        printf("Error opening or creating file '%s'\n", filename);
        exit(-1);
    }

    char buffer[MAX_LENGTH];
    int bytes;
    do {
        bytes = read(socketB, buffer, MAX_LENGTH);
        if (bytes > 0) {
            if (fwrite(buffer, bytes, 1, fd) < 0) {
                perror("fwrite");
                fclose(fd);
                return -1;
            }
        }
    } while (bytes > 0);
    fclose(fd);

    return readResponse(socketA, buffer);
}

int closeConnection(const int socketA, const int socketB) {
    char answer[MAX_LENGTH];
    write(socketA, "QUIT\r\n", 6);
    if(readResponse(socketA, answer) != 221) return -1; // 221 Service closing control connection.
    return close(socketA) || close(socketB);
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
    if (socketA < 0 || readResponse(socketA, answer) != 220) { // 220 Service ready for new user.
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
    if (response != 150) {
        printf("Unknown resource '%s' in '%s:%d'\n", url.path, ip, port);
        exit(-1);
    }
    int transferResponse = getResource(socketA, socketB, url.filename);
    printf("Response from getResource: %d\n", transferResponse);
    if (transferResponse != 226) {
        printf("Error transferring file '%s' from '%s:%d'\n", url.filename, ip, port);
        printf("Transfer response: %s\n", answer); // Adicione esta linha
        exit(-1);
    }

    if (closeConnection(socketA, socketB) != 0) {
        printf("Sockets close error\n");
        exit(-1);
    }

    printf("File downloaded successfully.\n");
    return 0;
}