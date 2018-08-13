#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <inttypes.h>
#include <crypt.h>

#define MAX_PASSWORD_LENGTH (7)
#define QUEUE_SIZE (4)
#define MAX_PREFIX (2)
#define BACKLOG 16
#define BUFFER 256
#define HASH_SIZE 13
#define ALPH_SIZE 255

#define TASK_TEMPLATE__(HASH_SIZE, ALPH_SIZE) \
  "<?xml version=\"1.0\"?>\n"                \
  "<msg_send>\n"                             \
  " <type>MT_SEND_JOB</type>\n"              \
  "   <args>\n"                              \
  "     <job>\n"                             \
  "       <job>\n"                           \
  "         <password>%s\n"                  \
  "          </password>\n"                  \
  "         <id>%d</id>\n"                   \
  "         <idx>%d</idx>\n"                 \
  "         <hash>%" #HASH_SIZE "s\n"        \
  "          </hash>\n"                      \
  "         <alphabet>%" #ALPH_SIZE "s\n"    \
  "          </alphabet>\n"                  \
  "         <from>%d</from>\n"               \
  "         <to>%d</to>\n"                   \
  "         <len>%d</len>\n"                 \
  "       </job>\n"                          \
  "     </job>\n"                            \
  "   </args>\n"                             \
  "</msg_send>\n"
 
#define TASK_TEMPLATE_(HASH_SIZE, ALPH_SIZE) TASK_TEMPLATE__ (HASH_SIZE, ALPH_SIZE)
#define TASK_TEMPLATE TASK_TEMPLATE_ (HASH_SIZE, ALPH_SIZE)
 
#define RESULT_TEMPLATE "<?xml version=\"1.0\"?>\n"\
  "<msg_recv>\n"                                   \
  "  <type>MT_REPORT_RESULTS</type>\n"             \
  "    <args>\n"                                   \
  "     <result>\n"                                \
  "      <result>\n"                               \
  "        <password>%s\n"                         \
  "         <password/>\n"                         \
  "        <id>%d</id>\n"                          \
  "        <idx>%d</idx>\n"                        \
  "        <password_found>%d\n"                   \
  "         </password_found>\n"                   \
  "      </result>\n"                              \
  "    </result>\n"                                \
  "  </args>\n"                                    \
  "</msg_recv>\n"

void error(const char *msg){
    perror(msg);
    exit(0);
}

typedef char password_t[MAX_PASSWORD_LENGTH + 1];

typedef enum {
    RM_SINGLE,
    RM_MULTI,
    RM_GENERATOR,
} run_mode_t;

typedef enum {
    BM_ITER,
    BM_REC,
} brute_mode_t;

typedef struct parmtrs_t {
    char * hostname;
    int host_socket;
    int client_id;
} parmtrs_t;

typedef struct net_context_t {
    int sockfd;
    int counter;
    pthread_mutex_t mutex;
    pthread_mutex_t sock_mutex;
} net_context_t;

typedef struct config_t {
    int portno;
    char * alph;
    int length;
    char * hash;
    brute_mode_t brute_mode;
    run_mode_t run_mode;
} config_t;

typedef struct result_t {
    bool found;
    password_t password;
} result_t;

typedef struct check_pass_args_t {
    result_t * result;
    char * hash;
    struct crypt_data data;
} check_pass_args_t;

typedef struct task_t {
    password_t password;
    int from;
    int to;
} task_t;

typedef bool (*password_handler_t) (task_t *, void *);

typedef bool (*rec_state_handler_t) (task_t *, void *);

typedef struct queue_t {
    task_t queue[QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t head_mutex, tail_mutex;
    sem_t full, empty;
} queue_t;

typedef struct context_t {
    config_t * config;
    result_t * result;
    queue_t queue;
} context_t;

typedef struct state_t {
    task_t * task;
    config_t * config;
    bool finished;
} state_t;

typedef struct iter_state_t {
    state_t state;
    int idx[MAX_PASSWORD_LENGTH + 1];
    int alph_len;
} iter_state_t;

typedef struct rec_state_t {
    ucontext_t reccontext, maincontext;
    char stack[MINSIGSTKSZ];
    state_t state;
} rec_state_t;

typedef struct sync_state_t {
    union{
        iter_state_t iter_state;
        rec_state_t rec_state;
        state_t state;
    };
    bool (*next)(state_t *);
    bool stop;
    pthread_mutex_t mutex;
} sync_state_t;

typedef struct gen_context_t {
    config_t * config;
    result_t * result;
    sync_state_t * sync_state;
} gen_context_t;

void serialize_task (char * pass, char * hash, char * alph, int id, int idx, int from, int to, int len, char * result){
    sprintf(result, TASK_TEMPLATE, pass, hash, alph, id, idx, from, to, len);
}

void deserialize_task (char * pass, char * hash, char * alph, int * id, int * idx, int * from, int * to, int * len, char * result){
    sscanf(result, TASK_TEMPLATE, pass, hash, alph, id, idx, from, to, len);
}
 
void serialize_result (char * pass, int id, int idx, int pass_found, char * result){
    sprintf(result, RESULT_TEMPLATE, pass, id, idx, pass_found);
}

void deserialize_result (char * pass, int * id, int * idx, int * pass_found, char * result){
    sprintf(result, RESULT_TEMPLATE, pass, id, idx, pass_found);
}

void queue_init (queue_t * queue)
{
    queue->head = 0;
    queue->tail = 0;
    pthread_mutex_init(&queue->head_mutex, NULL);
    pthread_mutex_init(&queue->tail_mutex, NULL);
    sem_init(&queue->full, 0, 0);
    sem_init(&queue->empty, 0, QUEUE_SIZE);
}

void queue_push (queue_t * queue, task_t * task)
{
    sem_wait(&queue->empty);
    pthread_mutex_lock(&queue->head_mutex);
    queue->queue[queue->head++] = *task;
    if (queue->head == QUEUE_SIZE)
        queue->head = 0;
    pthread_mutex_unlock(&queue->head_mutex);
    sem_post(&queue->full);
}

void queue_pop (queue_t * queue, task_t * task)
{
    sem_wait(&queue->full);
    pthread_mutex_lock(&queue->tail_mutex);
    *task = queue->queue[queue->tail++];
    if (queue->tail == QUEUE_SIZE)
        queue->tail = 0;
    pthread_mutex_unlock(&queue->tail_mutex);
    sem_post(&queue->empty);
}

bool check_pass (task_t * task, void * arg)
{
    check_pass_args_t * check_pass_args = arg;
    char * hash = check_pass_args->hash;
    if(strcmp(crypt_r(task->password, hash, &check_pass_args->data), hash) == 0)
    {
        strcpy(check_pass_args->result->password, task->password);
        check_pass_args->result->found = true;
        return true;
    }
    return false;
}

bool mt_password_handler (task_t * task, void * arg)
{
    context_t * context = arg;
    queue_push (&context->queue, task);
    return (context->result->found);
}

bool rec_state_handler(task_t * task, void * arg) {
    rec_state_t * rec_state = arg;
    swapcontext(&rec_state->reccontext, &rec_state->maincontext);
    return (rec_state->state.finished);
}

void iter_state_init(iter_state_t * iter_state, task_t * task, config_t * config) {
    iter_state->state.task = task;
    iter_state->state.config = config;
    iter_state->alph_len = strlen(config->alph) - 1;
    iter_state->state.finished = false;

    int i;
    for (i = iter_state->state.task->from; i < iter_state->state.task->to; i++)
    {
        iter_state->idx[i] = 0;
        iter_state->state.task->password[i] = (config->alph)[0];
    }
    iter_state->idx[iter_state->state.task->to - 1] = -1;
}

inline bool iter_state_next(state_t * state) {
    iter_state_t * iter_state = (iter_state_t*) state;
    if (iter_state->state.finished)
        return (true);

    int i;
    for (i = iter_state->state.task->to - 1; i >= iter_state->state.task->from && iter_state->idx[i] >= iter_state->alph_len; i--) 
    {
        iter_state->idx[i] = 0;
        iter_state->state.task->password[i] = iter_state->state.config->alph[0];
    }

    if (i < iter_state->state.task->from)
    {
        iter_state->state.finished = true;
        return true;
    }

    iter_state->state.task->password[i] = iter_state->state.config->alph[++iter_state->idx[i]];
    return false;
}

bool pass_mas(config_t * config, int pos, task_t * task, password_handler_t password_handler, void * arg){
    if(pos == task->to){
        return password_handler(task, arg);
    }
    else{
        char *a;
        for(a = config->alph; *a; ++a) {
            task->password[pos] = *a;
            if(pass_mas(config, pos + 1, task, password_handler, arg))
                return true;
        }
    }
    return false;
}

void brute_rec(config_t * config, task_t * task, password_handler_t password_handler, void * arg){
    pass_mas(config, task->from, task, password_handler, arg);
}

void brute_iter (config_t * config, task_t * task, password_handler_t password_handler, void * arg){
    iter_state_t iter_state;
    iter_state_init(&iter_state, task, config);

    for(;;){
        if(iter_state_next(&iter_state.state))
            break;

        if(password_handler(iter_state.state.task, arg))
            return;
    }
}

void rec_perform(rec_state_t * rec_state){
    brute_rec(rec_state->state.config, rec_state->state.task, rec_state_handler, rec_state);
    rec_state->state.finished = true;
}

void rec_state_init(rec_state_t * rec_state, config_t * config, task_t * task){
    rec_state->state.task = task;
    rec_state->state.config = config;
    rec_state->state.finished = false;
    
    rec_state->reccontext.uc_link = &rec_state->maincontext;
    rec_state->reccontext.uc_stack.ss_size = MINSIGSTKSZ;
    rec_state->reccontext.uc_stack.ss_flags = 0;
    rec_state->reccontext.uc_stack.ss_sp = rec_state->stack;
    
    rec_state->maincontext.uc_link = &rec_state->reccontext;
    rec_state->maincontext.uc_stack.ss_size = MINSIGSTKSZ;
    rec_state->maincontext.uc_stack.ss_flags = 0;
    rec_state->maincontext.uc_stack.ss_sp = rec_state->stack;
    
    makecontext(&rec_state->reccontext, (void (*) (void))rec_perform, 1, rec_state);
}

inline bool rec_state_next(state_t * state){
    rec_state_t * rec_state = (rec_state_t*) state;
    if (rec_state->state.finished)
        return (true);
    swapcontext(&rec_state->maincontext, &rec_state->reccontext);
    rec_perform(rec_state);
    return (rec_state->state.finished);
}

inline bool do_task(task_t * task, config_t * config, check_pass_args_t * check_pass_args){
    switch (config->brute_mode)
    {
        case BM_ITER:
            brute_iter(config, task, check_pass, check_pass_args);
            break;
        case BM_REC:
            brute_rec(config, task, check_pass, check_pass_args);
            break;
    }
    return check_pass_args->result->found;
}

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
    net_context_t * net_context = arg;
    int sockfd = net_context->sockfd;
    pthread_mutex_unlock(&net_context->sock_mutex);
    
    int i;
    for (i = 0; i < 100; i++){
        char msg[BUFFER];
        
        pthread_mutex_lock(&net_context->mutex);
        int counter = net_context->counter++;
        pthread_mutex_unlock(&net_context->mutex);
        
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

void * worker (void * arg)
{
    context_t * context = arg;
    check_pass_args_t check_pass_args = {
        .hash = context->config->hash,
        .result = context->result,
        .data.initialized = 0,
    };
    for (;;)
    {
        task_t task; 
        queue_pop (&context->queue, &task);
        if (!task.password[task.from])
            break;
            
        task.to = task.from;
        task.from = 0;
        do_task(&task, context->config, &check_pass_args);
    }
    return (NULL);
}

void * gen_worker(void * arg) {
    gen_context_t * gen_context = arg;
    check_pass_args_t check_pass_args = {
        .hash = gen_context->config->hash,
        .result = gen_context->result,
        .data.initialized = 0,
    };
	
    task_t task = {
        .from = 0,
        .to = gen_context->sync_state->state.task->from,
    };
    for(;;)
    {
        bool got_task = true;
        pthread_mutex_lock(&gen_context->sync_state->mutex);
        if (!gen_context->sync_state->next(&gen_context->sync_state->state))
            memcpy(task.password,gen_context->sync_state->state.task->password, sizeof (task.password));
       else{
            gen_context->sync_state->stop = true;
            got_task = false;
        }
        pthread_mutex_unlock(&gen_context->sync_state->mutex);

        if(got_task && do_task(&task, gen_context->config, &check_pass_args))
            gen_context->sync_state->stop = true;

        if (gen_context->sync_state->stop)
            break;
    }
    return NULL;
}

void run_single(config_t * config, result_t * result){
    check_pass_args_t check_pass_args = {
        .result = result,
        .hash = config->hash,
    };

    task_t task = {
        .from = 0,
        .to = config->length,
    };
    task.password[config->length] = 0;
    do_task(&task, config, &check_pass_args);
}

void run_multi(config_t * config, result_t * result){
    
    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
       error("ERROR opening socket");
    struct sockaddr_in serv_addr;
    struct sockaddr_in cli_addr;
    memset((char *) &serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = PF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(config->portno);
    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    listen(sockfd, BACKLOG);
    socklen_t clilen = sizeof(cli_addr);
    
    net_context_t net_context;
    net_context.counter = 0;
    pthread_mutex_init(&net_context.mutex, NULL);
    pthread_mutex_init(&net_context.sock_mutex, NULL);
    
    int i = 1;
    while (i > 0){
        int newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0)
            error("ERROR on accept");
        pthread_t comm;
        pthread_mutex_lock(&net_context.sock_mutex);
        net_context.sockfd = newsockfd;
        pthread_create(&comm, NULL, communicate, &net_context);
    }
    
    close(sockfd);
    
    int ncpu = 1; //(long)sysconf(_SC_NPROCESSORS_ONLN);

    context_t context = {
        .config = config,
        .result = result,
    };

    queue_init (&context.queue);

    pthread_t threads[ncpu];
    for (i = 0; i < ncpu; i++)
        pthread_create(&threads[i], NULL, worker, &context);

    task_t task = {
        .from = 2,
        .to = config->length,
    };
    task.password[config->length] = 0;

    switch (config->brute_mode)
    {
        case BM_ITER:
            brute_iter(config, &task, mt_password_handler, &context);
            break;
        case BM_REC:
            brute_rec(config, &task, mt_password_handler, &context);
            break;
    }

    task_t endtask;
    endtask.from = 0;
    endtask.password[endtask.from] = 0;
    for (i = 0; i < ncpu; i++)
        queue_push(&context.queue, &endtask);
}

void run_generator (config_t * config, result_t * result){
   int ncpu = (long)sysconf(_SC_NPROCESSORS_ONLN);

    sync_state_t sync_state;
    task_t task = {
        .from = 2,
        .to = config->length,
    };
    task.password[config->length] = 0;

    switch (config->brute_mode)
    {
        case BM_ITER:
            iter_state_init(&sync_state.iter_state, &task, config);
            sync_state.next = iter_state_next;
            break;
        case BM_REC:
            rec_state_init(&sync_state.rec_state, config, &task);
            sync_state.next = rec_state_next;
            break;
    }
    sync_state.stop = false;
    pthread_mutex_init(&sync_state.mutex, NULL);

    gen_context_t gen_context = {
        .config = config,
        .result = result,
        .sync_state = &sync_state,
    };

    int i;
    pthread_t threads[ncpu];
    for (i = 0; i < ncpu; i++)
        pthread_create(&threads[i], NULL, gen_worker, &gen_context);

    for (i = 0; i < ncpu; i++)
        pthread_join(threads[i], NULL);
}

void parse_params (config_t * config, int argc, char * argv[]){
    int c;
    while((c = getopt(argc, argv, "p:a:l:h:smgir")) != -1)
    {
        switch(c) {
            case 'p':
                config->portno = atoi(optarg);
                break;
            case 'a':
                config->alph = optarg;
                break;
            case 'l': 
                config->length = atoi (optarg);
                break;
            case 'h':
                config->hash = optarg;
                break;
            case 's':
                config->run_mode = RM_SINGLE;
                break;
            case 'm':
                config->run_mode = RM_MULTI;
                break;
            case 'g':
                config->brute_mode = RM_GENERATOR;
                break;    
            case 'i':
                config->brute_mode = BM_ITER;
                break;
            case 'r':
                config->brute_mode = BM_REC;
                break;
            case '?':
                break;
            default:
                break;
        }
    }
}

int main (int argc, char * argv[]) {
    if (argc < 2) {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }
    
    config_t config = {
        .portno = 0,
        .alph = "abc",
        .length = 0,
        .hash = NULL,
        .brute_mode = BM_ITER,
        .run_mode = RM_SINGLE,
    };
    result_t result = {
        .password = "",
        .found = false,
    };

    parse_params(&config, argc, argv);
    
    switch (config.run_mode)
    {
        case RM_SINGLE:
            run_single (&config, &result);
            break;
        case RM_MULTI:
            run_multi (&config, &result);
            break;
        case RM_GENERATOR:
            run_generator (&config, &result);
            break;
    }
    
    if (result.found) {
        printf ("The password is: %s\n", result.password);
    }
    else {
        printf ("Password wasn't found\n");
    }
    
    return (EXIT_SUCCESS);
}
