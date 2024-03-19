#include "queue.h"
#include <stdlib.h>
#include <threads.h>
#include <stdatomic.h>

// Structure for the nodes in the queue
typedef struct Node {
    void* item;
    struct Node* next;
} Node;

// Volatile pointers for the head and tail of the queue
static Node* volatile queue_head = NULL;
static Node* volatile queue_tail = NULL;

// Mutex and condition variable for thread synchronization
static mtx_t queue_mutex;
static cnd_t queue_condition;

// Atomic counters for queue size, waiting threads, and visited items
static atomic_size_t queue_item_count = 0;
static atomic_size_t queue_waiting_count = 0;
static atomic_size_t queue_visited_count = 0;

// Helper function to create a new node
static Node* createNode(void* item) {
    Node* new_node = malloc(sizeof(Node));
    if (new_node == NULL) {
        // Handle memory allocation failure here
        exit(EXIT_FAILURE);
    }
    new_node->item = item;
    new_node->next = NULL;
    return new_node;
}

// Helper function to destroy the queue
static void destroyQueueHelper(void) {
    Node* temp;
    while (queue_head != NULL) {
        temp = queue_head;
        queue_head = queue_head->next;
        free(temp);
    }
    queue_tail = NULL;
}

// Initialize the queue
void initQueue(void) {
    queue_head = queue_tail = NULL;
    mtx_init(&queue_mutex, mtx_plain);
    cnd_init(&queue_condition);
    queue_item_count = 0;
    queue_waiting_count = 0;
    queue_visited_count = 0;
}

// Destroy the queue
void destroyQueue(void) {
    destroyQueueHelper();
    mtx_destroy(&queue_mutex);
    cnd_destroy(&queue_condition);
}

// Add an item to the queue
void enqueue(void* item) {
    Node* new_node = createNode(item);

    mtx_lock(&queue_mutex);
    if (queue_tail == NULL) {
        queue_head = queue_tail = new_node;
    } else {
        queue_tail->next = new_node;
        queue_tail = new_node;
    }
    atomic_fetch_add(&queue_item_count, 1);
    cnd_broadcast(&queue_condition); // Signal that the queue is not empty
    mtx_unlock(&queue_mutex);
}

// Remove an item from the queue
void* dequeue(void) {
    mtx_lock(&queue_mutex);
    while (queue_head == NULL) {
        atomic_fetch_add(&queue_waiting_count, 1);
        cnd_wait(&queue_condition, &queue_mutex);
        atomic_fetch_sub(&queue_waiting_count, 1);
    }
    Node* temp = queue_head;
    void* item = temp->item;
    queue_head = queue_head->next;
    if (queue_head == NULL) {
        queue_tail = NULL;
    }
    free(temp);
    atomic_fetch_sub(&queue_item_count, 1);
    atomic_fetch_add(&queue_visited_count, 1);
    mtx_unlock(&queue_mutex);
    return item;
}

// Try to remove an item from the queue without blocking
bool tryDequeue(void** item) {
    if (mtx_trylock(&queue_mutex) == thrd_success) {
        if (queue_head == NULL) {
            mtx_unlock(&queue_mutex);
            return false;
        }
        Node* temp = queue_head;
        *item = temp->item;
        queue_head = queue_head->next;
        if (queue_head == NULL) {
            queue_tail = NULL;
        }
        free(temp);
        atomic_fetch_sub(&queue_item_count, 1);
        atomic_fetch_add(&queue_visited_count, 1);
        mtx_unlock(&queue_mutex);
        return true;
    }
    return false;
}

// Get the current size of the queue
size_t size(void) {
    return atomic_load(&queue_item_count);
}

// Get the number of threads waiting for the queue to fill
size_t waiting(void) {
    return atomic_load(&queue_waiting_count);
}

// Get the number of items that have passed through the queue
size_t visited(void) {
    return atomic_load(&queue_visited_count);
}
