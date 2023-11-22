#ifndef SAFEQUEUE_H
#define SAFEQUEUE_H

#include <semaphore.h>

// Define the QueueItem structure
typedef struct QueueItem {
    int client_fd;       // the client file descriptor
    char *path;          // the HTTP request or path
    int priority;        // the priority level
    int delay;           // the delay in seconds
    struct QueueItem *next; // pointer to the next item in the list
} QueueItem;

// Define the PriorityQueue structure
typedef struct {
    QueueItem *head; // pointer to the first item in the list
    sem_t empty;
    sem_t full;
    sem_t mutex;
} PriorityQueue;

// Function prototypes
PriorityQueue *create_queue(int max);
void add_work(PriorityQueue *pq, int client_fd, char *path, int priority, int delay);
QueueItem *get_work(PriorityQueue *pq);
QueueItem *get_work_nonblocking(PriorityQueue *pq);

#endif // SAFEQUEUE_H
