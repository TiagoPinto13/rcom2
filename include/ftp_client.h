#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Structure to store URL information
typedef struct {
    char user[256];        // Username
    char password[256];    // Password
    char host[256];        // Server hostname or IP address
    char path[512];        // Resource path
    char filename[256];    // Local filename to save the file
    char ip[INET_ADDRSTRLEN]; // Resolved IP address
} URL;

// Main functions
int parse_url(const char *url, URL *parsed_url);
int resolve_host(const char *host, char *ip);
int connect_to_server(const char *ip, int port);
int send_command(int sockfd, const char *command, char *response, size_t response_size);
int download_file(const URL *url_info);

#endif // DOWNLOAD_H