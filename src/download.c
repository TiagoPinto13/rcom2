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
#include <time.h>

#define SERVER_PORT 21
#define MAX_BUFFER_SIZE 1024

// Function to send FTP command and receive response
int send_command(int sockfd, const char *cmd, char *response, size_t response_len) {
    snprintf(response, response_len, "%s\r\n", cmd);
    if (write(sockfd, response, strlen(response)) < 0) {
        perror("write");
        return -1;
    }

    // Clear response buffer and wait for server response
    memset(response, 0, response_len);
    if (read(sockfd, response, response_len - 1) < 0) {
        perror("read");
        return -1;
    }

    return 0;
}

// Function to parse the passive mode response
int parse_pasv_response(const char *response, char *ip, int *port) {
    int p1, p2, p3, p4, p5, p6;
    if (sscanf(response, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).", &p1, &p2, &p3, &p4, &p5, &p6) == 6) {
        snprintf(ip, 16, "%d.%d.%d.%d", p1, p2, p3, p4);
        *port = p5 * 256 + p6;
        return 0;
    }
    return -1;
}

// Function to download file from the FTP server
int download_file(int control_sockfd, const char *file_name, const char *local_file) {
    char response[MAX_BUFFER_SIZE];
    char passive_ip[16];
    int passive_port;

    // Request passive mode
    if (send_command(control_sockfd, "PASV", response, sizeof(response)) < 0) {
        fprintf(stderr, "Failed to send PASV command\n");
        return -1;
    }

    // Parse the PASV response to get the server's IP and port for data connection
    if (parse_pasv_response(response, passive_ip, &passive_port) < 0) {
        fprintf(stderr, "Failed to parse PASV response\n");
        return -1;
    }

    // Create data socket
    int data_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in data_addr;
    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(passive_port);
    if (inet_pton(AF_INET, passive_ip, &data_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(data_sockfd);
        return -1;
    }

    // Connect to the data port
    if (connect(data_sockfd, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        perror("connect");
        close(data_sockfd);
        return -1;
    }

    // Send RETR command to start file transfer
    snprintf(response, sizeof(response), "RETR %s\r\n", file_name);
    if (send_command(control_sockfd, response, response, sizeof(response)) < 0) {
        fprintf(stderr, "Failed to send RETR command\n");
        close(data_sockfd);
        return -1;
    }

    // Open local file for writing
    int local_fd = open(local_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (local_fd < 0) {
        perror("open");
        close(data_sockfd);
        return -1;
    }

    // Read data from the server and write to local file
    char buffer[MAX_BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(data_sockfd, buffer, sizeof(buffer))) > 0) {
        if (write(local_fd, buffer, bytes_read) != bytes_read) {
            perror("write to file");
            close(data_sockfd);
            close(local_fd);
            return -1;
        }
    }

    if (bytes_read < 0) {
        perror("read from data socket");
    }

    // Close the file and data connection
    close(local_fd);
    close(data_sockfd);

    // Check for final response from RETR command
    if (send_command(control_sockfd, "QUIT", response, sizeof(response)) < 0) {
        fprintf(stderr, "Failed to send QUIT command\n");
        return -1;
    }

    return 0;
}

// Main function to execute FTP download
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <FTP server address> <file to download>\n", argv[0]);
        exit(-1);
    }

    const char *server_ip = argv[1];
    const char *file_name = argv[2];
    char local_file[MAX_BUFFER_SIZE];

    // Ask the user for the local file name to save the downloaded file
    printf("Enter the name to save the file as: ");
    if (fgets(local_file, sizeof(local_file), stdin) == NULL) {
        perror("fgets");
        exit(-1);
    }

    // Remove newline character from file name
    local_file[strcspn(local_file, "\n")] = 0;

    // Create control socket and connect to FTP server
    int control_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (control_sockfd < 0) {
        perror("socket");
        exit(-1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(control_sockfd);
        exit(-1);
    }

    if (connect(control_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(control_sockfd);
        exit(-1);
    }

    // Send USER and PASS commands
    if (send_command(control_sockfd, "USER anonymous", NULL, 0) < 0 ||
        send_command(control_sockfd, "PASS anonymous@", NULL, 0) < 0) {
        fprintf(stderr, "Failed to authenticate\n");
        close(control_sockfd);
        exit(-1);
    }

    // Start file download
    if (download_file(control_sockfd, file_name, local_file) < 0) {
        fprintf(stderr, "Failed to download file\n");
        close(control_sockfd);
        exit(-1);
    }

    // Close control socket
    close(control_sockfd);
    printf("File downloaded successfully!\n");

    return 0;
}
