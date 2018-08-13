#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

#define QUEUE_SIZE (4)

typedef struct queue_t {
    int queue[QUEUE_SIZE];
    int head, tail;
    pthread_mutex_t head_mutex, tail_mutex;
    sem_t full, empty;
} queue_t;

void queue_init (queue_t * queue)
{
    queue->head = 0;
    queue->tail = 0;
    pthread_mutex_init(&queue->head_mutex, NULL);
    pthread_mutex_init(&queue->tail_mutex, NULL);
    sem_init(&queue->full, 0, 0);
    sem_init(&queue->empty, 0, QUEUE_SIZE);
}

void queue_push (queue_t * queue, int * value)
{
    sem_wait(&queue->empty);
    pthread_mutex_lock(&queue->head_mutex);
    queue->queue[queue->head] = *value;
    if (++queue->head == QUEUE_SIZE)
        queue->head = 0;
    pthread_mutex_unlock(&queue->head_mutex);
    sem_post(&queue->full);
}

void queue_pop (queue_t * queue, int * value)
{
    sem_wait(&queue->full);
    pthread_mutex_lock(&queue->tail_mutex);
    *value = queue->queue[queue->tail];
    if (++queue->tail == QUEUE_SIZE)
        queue->tail = 0;
    pthread_mutex_unlock(&queue->tail_mutex);
    sem_post(&queue->empty);
}

void * worker (void * arg)
{
    queue_t * queue =  (queue_t*) arg;
    for (;;)
    {
        int value;
        queue_pop (queue, &value);
        printf("worker %d\n", value);
    }
}

int main (int argc, char * argv[])
{
    queue_t queue;
    pthread_t work1, work2;
    queue_init(&queue);
    
    pthread_create (&work1, NULL, worker, &queue);
    pthread_create (&work2, NULL, worker, &queue);
    
    int i;
    for (i = 0; ; ++i)
      queue_push (&queue, &i);
      
    return (EXIT_SUCCESS);
}
