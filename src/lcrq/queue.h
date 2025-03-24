#ifndef QUEUE_H
#define QUEUE_H

#include "lcrq.h"

void queue_init(queue_t * q, int nprocs);
void queue_free(queue_t * q, handle_t * h);
void handle_free(handle_t *h);


/* INTERFACE FOR 2D TESTING FRAMEWORK */
#define DS_ADD(s,k,v)       enqueue_wrap(s, k)
#define DS_REMOVE(q)        dequeue_wrap(q)
#define DS_SIZE(s)          lcrq_queue_size(s)
#define DS_NEW(w,c)         queue_create()
#define DS_REGISTER(s,i)    queue_register(s,i)

#define DS_TYPE             queue_t
#define DS_HANDLE           queue_t*
#define DS_NODE             sval_t

#define EMPTY						((sval_t)0)

extern __thread handle_t handle;

// Expose functions
int enqueue_wrap(queue_t *q, sval_t v);
sval_t dequeue_wrap(queue_t *q);
uint64_t lcrq_queue_size(queue_t *q);
uint64_t lcrq_enq_count(queue_t *q);
uint64_t lcrq_deq_count(queue_t *q);
uint64_t lcrq_tail_version(queue_t *q);
queue_t *queue_create();
queue_t* queue_register(queue_t* set, int thread_id);

/* End of interface */

#endif /* end of include guard: QUEUE_H */
