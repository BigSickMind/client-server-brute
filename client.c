#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <inttypes.h>

#define BACKLOG 16
#define BUFFER 256

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

typedef struct parmtrs_t {
    char * hostname;
    int host_socket;
} parmtrs_t;

int main(int argc, char *argv[]){
    parmtrs_t * parmtrs = (parmtrs_t *) arg;

    int portno = parmtrs->host_socket;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    struct hostent * server;
    server = gethostbyname(parmtrs->hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host\n");
        exit(0);
    }

    struct sockaddr_in * serv_addr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr->sin_family = AF_INET;
    memcpy((char *)&serv_addr->sin_addr.s_addr, (char *)server->h_addr, server->h_length);
    serv_addr->sin_port = htons(portno);
    if (connect(sockfd,(struct sockaddr *) serv_addr,sizeof(serv_addr)) < 0)
        error("ERROR connecting");
    
    
    int counter;
    for (;;){
        char buffer[BUFFER];
        int clt_sock = read(sockfd, buffer, sizeof(buffer));
        int32_t msg_len = strlen(buffer);
        char msg[msg_len];
        clt_sock = read(sockfd, msg, msg_len);
        if (clt_sock < 0)
            error("ERROR reading from socket");
        printf("Here is the message from server: %s\n", msg);
        
        counter = atoi(msg);
        counter++;
        
        char letter[BUFFER];
        msg_len = strlen(letter) + 1;
        sprintf(letter, "%d", counter);
        
        clt_sock = write(sockfd, &msg_len, sizeof(msg_len));
        clt_sock = write(sockfd, &letter, msg_len);
        if (clt_sock < 0){
            error("ERROR writing to socket");
        }
    }
    
    close(sockfd);
}
/*
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }

    int portno;

    pthread_t client_p;
    parmtrs_t parmtrs;
    parmtrs.hostname = "localhost";
    parmtrs.host_socket = portno = atoi(argv[1]);

    pthread_create(&client_p, NULL, client, &parmtrs);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
       error("ERROR opening socket");
    struct sockaddr_in * serv_addr;
    struct sockaddr_in * cli_addr;
    bzero((char *) serv_addr, sizeof(serv_addr));

    serv_addr->sin_family = AF_INET;
    serv_addr->sin_addr.s_addr = INADDR_ANY;
    serv_addr->sin_port = htons(portno);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, BACKLOG);
    socklen_t clilen = sizeof(cli_addr);
    int newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
    if (newsockfd < 0)
        error("ERROR on accept");
    
    int counter = 0;
    for (;;){
        char msg[BUFFER];
        int32_t msg_len = strlen(msg) + 1;
        sprintf(msg, "%d", counter);
        
        int serv_sock = write(newsockfd, &msg_len, sizeof(msg_len));
        serv_sock = write(newsockfd,  &msg, msg_len);
        if (serv_sock < 0){
            error("ERROR writing to socket");
        }
        
        char buffer[BUFFER];
        serv_sock = read(newsockfd, buffer, sizeof(buffer));
        msg_len = strlen(buffer);
        char letter[msg_len];
        serv_sock = read(newsockfd, letter, msg_len);
        
        if (serv_sock < 0)
            error("ERROR reading from socket");
        printf("Here is the message from client: %s\n", letter);
        
        counter = atoi(letter);
        counter++;
    }

    pthread_join(client_p, NULL);
    close(newsockfd);
    close(sockfd);
}*/
