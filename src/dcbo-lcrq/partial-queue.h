#ifndef QUEUE_H
#define QUEUE_H

#include "lcrq.h"

void queue_init(queue_t * q, int nprocs);
void queue_register(queue_t * q, handle_t * th, int id);
void queue_free(queue_t * q, handle_t * h);
void handle_free(handle_t *h);


/* INTERFACE FOR 2D TESTING FRAMEWORK */

// Define generics for d-balanced-queue
#define PARTIAL_T                   queue_t
#define PARTIAL_ENQUEUE(q, k, v)    enqueue_wrap(q, &lcrq_handle, v)
#define PARTIAL_DEQUEUE(q)          dequeue_wrap(q, &lcrq_handle)
#define INIT_PARTIAL(q,n)           queue_init(q,n)
#define PARTIAL_LENGTH(q)           lcrq_queue_size(q)
#define PARTIAL_TAIL_VERSION(q)     lcrq_tail_version(q)
#define PARTIAL_ENQ_COUNT(q)        lcrq_enq_count(q)
#define PARTIAL_DEQ_COUNT(q)        lcrq_deq_count(q)
#define EMPTY						((sval_t)0)

extern __thread handle_t lcrq_handle;

// Expose functions
int enqueue_wrap(queue_t *q, handle_t *th, sval_t v);
sval_t dequeue_wrap(queue_t *q, handle_t *th);
uint64_t lcrq_queue_size(queue_t *q);
uint64_t lcrq_enq_count(queue_t *q);
uint64_t lcrq_deq_count(queue_t *q);
uint64_t lcrq_tail_version(queue_t *q);

/* End of interface */

#endif /* end of include guard: QUEUE_H */
