#include "queue.h"
#include <threads.h>
#include <stdatomic.h>
#include <stdlib.h>


// Oversees the management of a thread queue, keeping tabs on the head and tail, as well as the tally of threads lined up for processing.
struct ThreadQueue
{
    struct QueueNode *head;
    struct QueueNode *tail;
    atomic_ulong waiting_thread_count;
};

// Describes an individual thread node within the queue, detailing its unique ID, successor, synchronization condition variable, completion state, and condition to wait for.
struct QueueNode
{
    thrd_t id;
    struct QueueNode *successor;
    // Dedicated condition variable for selective thread notification.
    cnd_t sync_condition;
    bool terminated;
    int wait_condition;
};

// Organizes a queue for generic data items, containing pointers to the first and last entries, and maintains metrics for total size, number of processed items, and quantity of items entered.
struct DataQueue
{
    struct DataElement *head;
    struct DataElement *tail;
    atomic_ulong total_size;
    atomic_ulong items_processed;
    atomic_ulong items_enqueued;
    mtx_t synchronization_lock;
};

// Characterizes an individual node within the data queue, holding a reference to the subsequent node, an identifier for the data, and the pointer to the data itself.
struct DataElement
{
    struct DataElement *next;
    int index;
    void *pointer;
};



static struct ThreadQueue thread_queue;
static struct DataQueue data_queue;
static thrd_t current_thread;


void removeAllDataElements(void);
void teardownThreadQueue(void);
struct DataElement *createDataElement(void *data);
void appendToDataQueue(struct DataElement *elementToAdd);
void appendToEmptyDataQueue(struct DataElement *elementToAdd);
void appendToPopulatedDataQueue(struct DataElement *elementToAdd);
bool checkIfThreadShouldYield(void);
void enqueueQueueNode(void);
void dequeueQueueNode(void);
void appendToThreadQueue(struct QueueNode *nodeToAdd);
void appendToEmptyThreadQueue(struct QueueNode *nodeToAdd);
void appendToPopulatedThreadQueue(struct QueueNode *nodeToAdd);
struct QueueNode *createThreadQueueNode(void);
int fetchFirstWaitConditionStatus(void);


void initQueue(void)
{
    // Set pointers in the data queue to NULL, preparing for an empty queue state.
    dataQueue.head = NULL;
    dataQueue.tail = NULL;
    // Initialize all counters in the data queue to 0 for a clear start.
    dataQueue.total_size = 0;
    dataQueue.items_processed = 0;
    dataQueue.items_enqueued = 0;
    // Prepare the mutex for future operations on the data queue.
    mtx_init(&dataQueue.synchronization_lock, mtx_plain);
    
    // Set thread queue pointers to NULL, indicating absence of enqueued threads.
    threadQueue.head = NULL;
    threadQueue.tail = NULL;
    // Initialize the count of threads in waiting to 0.
    threadQueue.waiting_thread_count = 0;
}

void destroyQueue(void)
{
    // Obtain exclusive access to the data queue by locking it.
    mtx_lock(&dataQueue.synchronization_lock);
    // Perform a secure cleanup of data nodes.
    removeAllDataElements();
    // Methodically dismantle the thread queue.
    teardownThreadQueue();
    // Unlock the data queue after finishing cleanup activities.
    mtx_unlock(&dataQueue.synchronization_lock);
    // Dispose of the mutex as the data queue is no longer required.
    mtx_destroy(&dataQueue.synchronization_lock);
}

void removeAllDataElements(void)
{
    struct DataElement *current_element;
    while (dataQueue.head != NULL)
    {
        current_element = dataQueue.head;
        dataQueue.head = current_element->next;
        free(current_element);
    }
    // Clear remaining fields to maintain a consistent state for the data queue.
    dataQueue.tail = NULL;
    dataQueue.total_size = 0;
    dataQueue.items_processed = 0;
    dataQueue.items_enqueued = 0;
}

void teardownThreadQueue(void)
{
    thrd_t current_thread = thrd_current();
    while (threadQueue.head != NULL)
    {
        threadQueue.head->terminated = true;
        cnd_signal(&threadQueue.head->sync_condition);
        // Advance to the next node to prevent looping indefinitely.
        threadQueue.head = threadQueue.head->successor;
    }
    // Restore the thread queue to an initial state after emptying it.
    threadQueue.tail = NULL;
    threadQueue.waiting_thread_count = 0;
}

void enqueue(void *data)
{
    mtx_lock(&dataQueue.synchronization_lock);
    struct DataElement *new_element = createDataElement(data);
    appendToDataQueue(new_element);
    mtx_unlock(&dataQueue.synchronization_lock);

    if (dataQueue.total_size > 0 && threadQueue.waiting_thread_count > 0)
    {
        // Trigger the first thread in the queue if there are items and threads are waiting.
        cnd_signal(&threadQueue.head->sync_condition);
    }
}

struct DataElement *createDataElement(void *data)
{
    // Assume successful memory allocation as per the given context.
    struct DataElement *element = (struct DataElement *)malloc(sizeof(struct DataElement));
    element->pointer = data;
    element->next = NULL;
    element->index = dataQueue.items_enqueued;
    return element;
}

void addToDataQueue(struct DataElement *elementToAdd)
{
    dataQueue.total_size == 0 ? appendToEmptyDataQueue(elementToAdd) : appendToPopulatedDataQueue(elementToAdd);
}

void appendToEmptyDataQueue(struct DataElement *elementToAdd)
{
    dataQueue.head = elementToAdd;
    dataQueue.tail = elementToAdd;
    dataQueue.total_size++;
    dataQueue.items_enqueued++;
}

void appendToPopulatedDataQueue(struct DataElement *elementToAdd)
{
    dataQueue.tail->next = elementToAdd;
    dataQueue.tail = elementToAdd;
    dataQueue.total_size++;
    dataQueue.items_enqueued++;
}

void *dequeue(void)
{
    mtx_lock(&dataQueue.synchronization_lock);
    // Thread waits if necessary as per the conditions
    while (checkIfThreadShouldYield())
    {
        enqueueQueueNode();
        struct QueueNode *currentThreadNode = threadQueue.tail;
        cnd_wait(&currentThreadNode->sync_condition, &dataQueue.synchronization_lock);
        if (currentThreadNode->terminated)
        {
            struct QueueNode *previousHead;
            previousHead = threadQueue.head;
            threadQueue.head = previousHead->successor;
            // To prevent orphan threads when the queue is being destroyed
            free(previousHead);
            thrd_join(current_thread, NULL);
        }
        if (dataQueue.head && fetchFirstWaitConditionStatus() <= dataQueue.head->index)
        {
            dequeueQueueNode();
        }
    }

    struct DataElement *elementRemoved = dataQueue.head;
    dataQueue.head = elementRemoved->next;
    if (dataQueue.head == NULL)
    {
        dataQueue.tail = NULL;
    }
    dataQueue.total_size--;
    dataQueue.items_processed++;
    mtx_unlock(&dataQueue.synchronization_lock);
    void *data = elementRemoved->pointer;
    free(elementRemoved);
    return data;
}

bool checkIfThreadShouldYield(void)
{
    if (dataQueue.total_size == 0)
    {
        return true;
    }
    if (threadQueue.waiting_thread_count <= dataQueue.total_size)
    {
        return false;
    }
    int statusOfFirstWaiting = fetchFirstWaitConditionStatus();
    return statusOfFirstWaiting > dataQueue.head->index;
}

int fetchFirstWaitConditionStatus(void)
{
    struct QueueNode *threadNodeChecker = threadQueue.head;
    while (threadNodeChecker != NULL)
    {
        if (thrd_equal(thrd_current(), threadNodeChecker->thread_id))
        {
            return threadNodeChecker->waiting_on_index;
        }
        threadNodeChecker = threadNodeChecker->successor;
    }
    return -1;
}

void enqueueQueueNode(void)
{
    struct QueueNode *newQueueNode = createThreadQueueNode();
    appendToThreadQueue(newQueueNode);
}

void dequeueQueueNode(void)
{
    struct QueueNode *nodeBeingRemoved = threadQueue.head;
    threadQueue.head = nodeBeingRemoved->successor;
    free(nodeBeingRemoved);
    if (threadQueue.head == NULL)
    {
        threadQueue.tail = NULL;
    }
    threadQueue.waiting_thread_count--;
}

void appendToThreadQueue(struct QueueNode *nodeToAdd)
{
    threadQueue.waiting_thread_count == 0 ? appendToEmptyThreadQueue(nodeToAdd) : appendToPopulatedThreadQueue(nodeToAdd);
}

void appendToEmptyThreadQueue(struct QueueNode *nodeToAdd)
{
    threadQueue.head = nodeToAdd;
    threadQueue.tail = nodeToAdd;
    threadQueue.waiting_thread_count++;
}

void appendToPopulatedThreadQueue(struct QueueNode *nodeToAdd)
{
    threadQueue.tail->successor = nodeToAdd;
    threadQueue.tail = nodeToAdd;
    threadQueue.waiting_thread_count++;
}

struct QueueNode *createThreadQueueNode(void)
{
    struct QueueNode *newQueueNode = (struct QueueNode *)malloc(sizeof(struct QueueNode));
    newQueueNode->thread_id = thrd_current();
    newQueueNode->successor = NULL;
    newQueueNode->terminated = false;
    cnd_init(&newQueueNode->sync_condition);
    newQueueNode->waiting_on_index = dataQueue.items_enqueued + threadQueue.waiting_thread_count;
    return newQueueNode;
}

bool tryDequeue(void **dataPointer)
{
    mtx_lock(&dataQueue.synchronization_lock);
    if (dataQueue.total_size == 0 || dataQueue.head == NULL)
    {
        mtx_unlock(&dataQueue.synchronization_lock);
        return false;
    }
    struct DataElement *elementBeingRemoved = dataQueue.head;
    dataQueue.head = elementBeingRemoved->next;
    if (dataQueue.head == NULL)
    {
        dataQueue.tail = NULL;
    }
    dataQueue.total_size--;
    dataQueue.items_processed++;
    mtx_unlock(&dataQueue.synchronization_lock);
    *dataPointer = elementBeingRemoved->pointer;
    free(elementBeingRemoved);
    return true;
}

size_t size(void)
{
    return dataQueue.total_size;
}

size_t waiting(void)
{
    return threadQueue.waiting_thread_count;
}

size_t visited(void)
{
    return dataQueue.items_processed;
}
