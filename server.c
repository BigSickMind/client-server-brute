#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <inttypes.h>

#define BACKLOG 16
#define BUFFER 256

void error(const char *msg){
    perror(msg);
    exit(0);
}

typedef struct parmtrs_t {
    char * hostname;
    int host_socket;
    int client_id;
} parmtrs_t;

typedef struct context_t {
    int sockfd;
    int counter;
    pthread_mutex_t mutex;
    pthread_mutex_t sock_mutex;
} context_t;


void * client(void * arg) {
    parmtrs_t * parmtrs = (parmtrs_t *) arg;

    int portno = parmtrs->host_socket;

    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    struct hostent * server;
    server = gethostbyname(parmtrs->hostname);
    int client_id = parmtrs->client_id;
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host\n");
        exit(0);
    }

    struct sockaddr_in serv_addr;
    memset((char *) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy((char *) &serv_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);
    serv_addr.sin_port = htons(portno);
    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0)
        error("ERROR connecting");
    
    
    int counter;
    int i;
    for (i = 0; i < 100; i++){
        int32_t msg_len;
        int clt_sock = read(sockfd, &msg_len, sizeof(msg_len));
        char msg[msg_len];
        clt_sock = read(sockfd, &msg, msg_len);
        if (clt_sock < 0)
            error("ERROR reading from socket");
        //printf("Here is the message from server: %s\n", msg);
        
        counter = atoi(msg);
        counter++;
        int32_t id = client_id;
        char buffer[BUFFER];
        sprintf(buffer, "%d", counter);
        msg_len = strlen(buffer) + 1;
        clt_sock = write(sockfd, &id, sizeof(id));
        clt_sock = write(sockfd, &msg_len, sizeof(msg_len));
        clt_sock = write(sockfd, &buffer, msg_len);
        if (clt_sock < 0){
            error("ERROR writing to socket");
        }
    }
    
    close(sockfd);
    return (NULL);
}

void * communicate(void * arg){
    context_t * context = arg;
    int sockfd = context->sockfd;
    pthread_mutex_unlock(&context->sock_mutex);
    
    int i;
    for (i = 0; i < 100; i++){
        char msg[BUFFER];
        
        pthread_mutex_lock(&context->mutex);
        int counter = context->counter++;
        pthread_mutex_unlock(&context->mutex);
        
        sprintf(msg, "%d", counter);
        int32_t msg_len = strlen(msg) + 1;
        int serv_sock = write(sockfd, &msg_len, sizeof(msg_len));
        serv_sock = write(sockfd, &msg, msg_len);
        if (serv_sock < 0){
            error("ERROR writing to socket");
        }
        int32_t client_id;
        serv_sock = read(sockfd, &client_id, sizeof(client_id));
        serv_sock = read(sockfd, &msg_len, sizeof(msg_len));
        char buffer[msg_len];
        serv_sock = read(sockfd, buffer, msg_len);
        
        if (serv_sock < 0)
            error("ERROR reading from socket");
        printf("Here is the message from client %d: %s\n", client_id, buffer);
        
    }
    return (NULL);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }
    
    int portno;

    pthread_t client_p1, client_p2;
    parmtrs_t parmtrs1, parmtrs2;
    parmtrs1.hostname = parmtrs2.hostname = "localhost";
    parmtrs1.host_socket = parmtrs2.host_socket = portno = atoi(argv[1]);
    parmtrs1.client_id = 1;
    parmtrs2.client_id = 2;
    
    pthread_create(&client_p1, NULL, client, &parmtrs1);
    pthread_create(&client_p1, NULL, client, &parmtrs2);
    
    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
       error("ERROR opening socket");
    struct sockaddr_in serv_addr;
    struct sockaddr_in cli_addr;
    memset((char *) &serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = PF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, BACKLOG);
    socklen_t clilen = sizeof(cli_addr);
    
    context_t context;
    context.counter = 0;
    pthread_mutex_init(&context.mutex, NULL);
    pthread_mutex_init(&context.sock_mutex, NULL);
    
    int i = 1;
    while (i > 0){
        int newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0)
            error("ERROR on accept");
        pthread_t comm;
        pthread_mutex_lock(&context.sock_mutex);
        context.sockfd = newsockfd;
        pthread_create(&comm, NULL, communicate, &context);
    }

    pthread_join(client_p1, NULL);
    pthread_join(client_p2, NULL);
    close(sockfd);
    
    return (0);
}
