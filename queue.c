#include "queue.h"
#include <stdlib.h>
#include <threads.h>
#include <stdatomic.h>

// Node structure for the queue
typedef struct Node {
    void* data;
    struct Node* next;
} Node;

// Volatile pointers to head and tail of the queue
static Node* volatile head = NULL;
static Node* volatile tail = NULL;

// Mutex and condition variable for thread synchronization
static mtx_t queue_lock;
static cnd_t queue_not_empty;

// Atomic counters for queue size, waiting threads, and visited items
static atomic_size_t queue_size = 0;
static atomic_size_t queue_waiting = 0;
static atomic_size_t queue_visited = 0;

// Initialize the queue
void initQueue(void) {
    head = tail = NULL;
    mtx_init(&queue_lock, mtx_plain);
    cnd_init(&queue_not_empty);
    queue_size = 0;
    queue_waiting = 0;
    queue_visited = 0;
}

// Destroy the queue
void destroyQueue(void) {
    while (head != NULL) {
        dequeue(); // Dequeue all items to free memory
    }
    mtx_destroy(&queue_lock);
    cnd_destroy(&queue_not_empty);
}

// Create a new node
Node* createNode(void* data) {
    Node* new_node = malloc(sizeof(Node));
    if (new_node == NULL) {
        // Handle memory allocation failure here
        exit(EXIT_FAILURE);
    }
    new_node->data = data;
    new_node->next = NULL;
    return new_node;
}

// Enqueue an item
void enqueue(void* item) {
    Node* new_node = createNode(item);

    mtx_lock(&queue_lock);
    if (tail == NULL) {
        head = tail = new_node;
    } else {
        tail->next = new_node;
        tail = new_node;
    }
    atomic_fetch_add(&queue_size, 1);
    cnd_broadcast(&queue_not_empty); // Wake all waiting threads
    mtx_unlock(&queue_lock);
}

// Dequeue an item
void* dequeue(void) {
    mtx_lock(&queue_lock);
    while (head == NULL) {
        atomic_fetch_add(&queue_waiting, 1);
        cnd_wait(&queue_not_empty, &queue_lock);
        atomic_fetch_sub(&queue_waiting, 1);
    }
    Node* temp = head;
    void* data = temp->data;
    head = head->next;
    if (head == NULL) tail = NULL;
    free(temp);
    atomic_fetch_sub(&queue_size, 1);
    atomic_fetch_add(&queue_visited, 1);
    mtx_unlock(&queue_lock);
    return data;
}

// Try to dequeue an item without blocking
bool tryDequeue(void** item) {
    if (mtx_trylock(&queue_lock) == thrd_success) {
        if (head == NULL) {
            mtx_unlock(&queue_lock);
            return false;
        }
        Node* temp = head;
        *item = temp->data;
        head = head->next;
        if (head == NULL) tail = NULL;
        free(temp);
        atomic_fetch_sub(&queue_size, 1);
        atomic_fetch_add(&queue_visited, 1);
        mtx_unlock(&queue_lock);
        return true;
    }
    return false;
}

// Get the current size of the queue
size_t size(void) {
    return atomic_load(&queue_size);
}

// Get the number of waiting threads
size_t waiting(void) {
    return atomic_load(&queue_waiting);
}

// Get the number of visited items
size_t visited(void) {
    return atomic_load(&queue_visited);
}
