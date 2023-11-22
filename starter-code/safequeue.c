/*
create_queue(): Create a new priority queue.

add_work(): When a new request comes in, you will insert it in priority order.

get_work(): Get the job with highest priority.

You will need two versions of the remove functionality, one that will be called by worker threads when they want to get a new job to work on, and another that is called by listener threads when they receive a "GetJob" request.

get_work(): The worker threads will call a Blocking version of remove, where if there are no elements in the queue, they will block until an item is added. You can implement this using condition variables.

get_work_nonblocking(): The listener threads should call a Non-Blocking function to get the highest priority job. If there are no elements on the queue, they will simply return and send an error message to the client.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <unistd.h>
#include <semaphore.h>
#include "safequeue.h"

/*
typedef struct QueueItem
{
    char *path;          // the HTTP request or path
    int priority;           // the priority level
    int delay;              // the delay in seconds
    struct QueueItem *next; // pointer to the next item in the list
} QueueItem;

typedef struct
{
    QueueItem *head; // pointer to the first item in the list
    sem_t empty;
    sem_t full;
    sem_t mutex;
    // Include synchronization primitives like mutexes and condition variables
} PriorityQueue;
*/

PriorityQueue *create_queue(int max)
{
    PriorityQueue *pq = malloc(sizeof(PriorityQueue));
    pq->head = NULL;
    // Initialize mutexes and condition

    // Initialize semaphores
    if (sem_init(&pq->empty, 0, max) != 0)
    {
        perror("Failed to initialize empty semaphore");
        free(pq);
        return NULL;
    }

    if (sem_init(&pq->full, 0, 0) != 0)
    {
        perror("Failed to initialize full semaphore");
        sem_destroy(&pq->empty);
        free(pq);
        return NULL;
    }

    if (sem_init(&pq->mutex, 0, 1) != 0)
    {
        perror("Failed to initialize mutex semaphore");
        sem_destroy(&pq->full);
        sem_destroy(&pq->empty);
        free(pq);
        return NULL;
    }
    return pq;
}

void add_work(PriorityQueue *pq, int client_fd, char *path, int priority, int delay)
{
    QueueItem *item = malloc(sizeof(QueueItem));
    item->client_fd = client_fd;
    item->path = path;
    item->priority = priority;
    item->delay = delay;
    sem_wait(&pq->empty);
    sem_wait(&pq->mutex);
    if (pq->head == NULL || pq->head->priority < item->priority)
    {
        item->next = pq->head;
        pq->head = item;
    }
    else
    {
        QueueItem *current = pq->head;
        while (current->next != NULL && current->next->priority >= item->priority)
        {
            current = current->next;
        }
        item->next = current->next;
        current->next = item;
    }
    sem_post(&pq->mutex);
    sem_post(&pq->full);
}


QueueItem *get_work(PriorityQueue *pq)
{
    sem_wait(&pq->full);
    sem_wait(&pq->mutex);
    QueueItem *item = pq->head;
    pq->head = pq->head->next;
    sem_post(&pq->mutex);
    sem_post(&pq->empty);
    return item;
}

QueueItem *get_work_nonblocking(PriorityQueue *pq)
{
    if (sem_trywait(&pq->full) != 0) {
        // Queue is empty or could not acquire semaphore
        return NULL;
    }

    sem_wait(&pq->mutex);

    // Remove and return the first item in the list
    QueueItem *item = pq->head;
    if (item != NULL) {
        pq->head = pq->head->next;
    }

    sem_post(&pq->mutex);
    sem_post(&pq->empty);

    return item;
}
