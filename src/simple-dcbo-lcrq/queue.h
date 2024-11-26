#ifndef QUEUE_H
#define QUEUE_H

#include "lcrq.h"

void queue_init(queue_t * q, int nprocs);
void queue_register(queue_t * q, handle_t * th, int id);
void queue_free(queue_t * q, handle_t * h);
void handle_free(handle_t *h);


/* INTERFACE FOR 2D TESTING FRAMEWORK */

// Define generics for d-balanced-queue
//#define PARTIAL_T                   queue_t

#define LCRQ_PARTIAL_ENQUEUE(q, k, v)    enqueue_wrap(q, &lcrq_handle, v)
#define LCRQ_PARTIAL_DEQUEUE(q)       dequeue_wrap(q, &lcrq_handle)
#define EMPTY						((sval_t)0)

extern __thread handle_t handle;

// Expose functions
int enqueue_wrap(queue_t *q, handle_t *th, sval_t v);
int dequeue_wrap(queue_t *q, handle_t *th);
uint64_t lcrq_queue_size(queue_t *q);
uint64_t lcrq_enq_count(queue_t *q);
uint64_t lcrq_deq_count(queue_t *q);
uint64_t lcrq_tail_version(queue_t *q);

int dequeue_wrap(queue_t *q, handle_t *th);

/* End of interface */

#endif /* end of include guard: QUEUE_H */
