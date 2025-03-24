#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lcrq.h"
#include "align.h"
//#include "delay.h"
//#include "hzdptr.h"
//#include "primitives.h"

#ifdef RELAXATION_TIMER_ANALYSIS
#include "relaxation_analysis_timestamps.c"
#endif

#define RING_SIZE LCRQ_RING_SIZE

// Want timers at FAA increments and not with the normal CAE
#ifdef RELAXATION_TIMER_ANALYSIS
__thread uint64_t enq_timestamp, deq_timestamp;
#define ENQ_TIMESTAMP (enq_timestamp = get_timestamp())
#define DEQ_TIMESTAMP (deq_timestamp = get_timestamp())
#else
#define ENQ_TIMESTAMP
#define DEQ_TIMESTAMP
#endif

static inline int is_empty(uint64_t v) __attribute__ ((pure));
static inline uint64_t node_index(uint64_t i) __attribute__ ((pure));
static inline uint64_t set_unsafe(uint64_t i) __attribute__ ((pure));
static inline uint64_t node_unsafe(uint64_t i) __attribute__ ((pure));
static inline uint64_t tail_index(uint64_t t) __attribute__ ((pure));
static inline int crq_is_closed(uint64_t t) __attribute__ ((pure));

static inline void init_ring(RingQueue *r) {
  int i;

  for (i = 0; i < RING_SIZE; i++) {
    r->array[i].ring_node.val = -1;
    r->array[i].ring_node.idx = i;
  }

  r->head = r->tail = 0;
  r->next = NULL;
  r->items_enqueued = 0;
}

inline int is_empty(uint64_t v)  {
  return (v == (uint64_t)-1);
}


inline uint64_t node_index(uint64_t i) {
  return (i & ~(1ull << 63));
}


inline uint64_t set_unsafe(uint64_t i) {
  return (i | (1ull << 63));
}


inline uint64_t node_unsafe(uint64_t i) {
  return (i & (1ull << 63));
}


inline uint64_t tail_index(uint64_t t) {
  return (t & ~(1ull << 63));
}


inline int crq_is_closed(uint64_t t) {
  return (t & (1ull << 63)) != 0;
}

static int enq_cae(volatile RingNode* item_loc, RingNode* expected, RingNode* new_value)
{
	//sval_t expected = EMPTY;
#ifdef RELAXATION_TIMER_ANALYSIS
	// Use timers to track relaxation instead of locks
	if (CAE(item_loc, expected, new_value))
	{
		// Save this count in a local array of (timestamp, )
		add_relaxed_put(new_value->val, enq_timestamp);
		return true;
	}
	return false;
#else
	return CAE(item_loc, expected, new_value);
#endif
}

static int deq_cae(volatile RingNode* item_loc, RingNode* expected, RingNode* new_value)
{
	//sval_t expected = EMPTY;
#ifdef RELAXATION_TIMER_ANALYSIS
	// Use timers to track relaxation instead of locks
	if (CAE(item_loc, expected, new_value))
	{
		// Save this count in a local array of (timestamp, )
		add_relaxed_get(expected->val, deq_timestamp);
		return true;
	}
	return false;
#else
	return CAE(item_loc, expected, new_value);
#endif
}

void queue_init(queue_t * q, int nprocs)
{
  RingQueue *rq = (RingQueue*) ssalloc_aligned(CACHE_LINE_SIZE, sizeof(RingQueue));
  //RingQueue *rq = align_malloc(PAGE_SIZE, sizeof(RingQueue));
  init_ring(rq);

  q->head = rq;
  q->tail = rq;
  //q->nprocs = nprocs;
}

static inline void fixState(RingQueue *rq) {

  while (1) {
    uint64_t t = rq->tail;
    uint64_t h = rq->head;

    if (rq->tail != t)
      continue;

    if (h > t) {
      if (CAE(&rq->tail, &t, &h)) break;
      continue;
    }
    break;
  }
}
//Had to make some changes here to make new_value CAE work!!
static inline int close_crq(RingQueue *rq, const uint64_t t, const int tries) {
  uint64_t tt = t + 1;

  if (tries < 10) {
    uint64_t nt = tt|1ull<<63;
    return CAE(&rq->tail, &tt, &nt);}
  else
    return TAS_U64(&rq->tail, 63);
}

static void lcrq_put(queue_t * q, handle_t * handle, uint64_t arg) {
  int try_close = 0;

  while (1) {
    //RingQueue *rq = hzdptr_setv(&q->tail, &handle->hzdptr, 0);
    RingQueue *rq = q->tail;

    RingQueue *next = rq->next;

    if (next != NULL) {
      CAE(&q->tail, &rq, &next);
      continue;
    }

    uint64_t t = FAI_U64(&rq->tail);
    ENQ_TIMESTAMP;

    if (crq_is_closed(t)) {
      RingQueue * nrq;
alloc:
      nrq = handle->next;

      if (nrq == NULL) {
        //nrq = align_malloc(PAGE_SIZE, sizeof(RingQueue));
        nrq = (RingQueue*) ssmem_alloc(alloc, sizeof(RingQueue));
        init_ring(nrq);
      }

      // Solo enqueue
      nrq->tail = 1;
      nrq->array[0].ring_node.val = (uint64_t) arg;
      nrq->array[0].ring_node.idx = 0;
      nrq->items_enqueued = rq->items_enqueued + tail_index(t);

      if (CAE(&rq->next, &next, &nrq)) {
        CAE(&q->tail, &rq, &nrq);
        #ifdef RELAXATION_TIMER_ANALYSIS
		      add_relaxed_put(arg, enq_timestamp);
        #endif
        handle->next = NULL;
        return;
      }
      //Shared between other queues!
      handle->next = nrq;
      continue;
    }

    RingNode cell = rq->array[t & (RING_SIZE-1)].ring_node;

    uint64_t idx = cell.idx;
    uint64_t val = cell.val;

    if (is_empty(val)) {
      if (node_index(idx) <= t) {
        RingNode new_value_ring_node;
        new_value_ring_node.val = arg;
        new_value_ring_node.idx = t;
        if ((!node_unsafe(idx) || rq->head < t) &&
            enq_cae(&rq->array[t & (RING_SIZE-1)].ring_node, &cell, &new_value_ring_node)) {
          return;
        }
      }
    }

    uint64_t h = rq->head;

    if ((int64_t)(t - h) >= (int64_t)RING_SIZE &&
        close_crq(rq, t, ++try_close)) {
      goto alloc;
    }
  }

  //hzdptr_clear(&handle->hzdptr, 0);
}

static uint64_t lcrq_get(queue_t * q, handle_t * handle) {
  while (1) {
    //RingQueue *rq = hzdptr_setv(&q->head, &handle->hzdptr, 0);
    RingQueue *rq = q->head;
    RingQueue *next;

    // Not in the paper, but added for better performance at nearly empty queues
    // Requires x86 memory order and volatile to not re-order these two reads
    int64_t head = rq->head;
    int64_t tail = rq->tail;
    if (head >= tail && rq->next == NULL) return -1;

    uint64_t h = FAI_U64(&rq->head);
    DEQ_TIMESTAMP;

    RingNode cell = rq->array[h & (RING_SIZE-1)].ring_node;

    uint64_t tt = 0;
    int r = 0;

    while (1) {

      uint64_t cell_idx = cell.idx;
      uint64_t unsafe = node_unsafe(cell_idx);
      uint64_t idx = node_index(cell_idx);
      uint64_t val = cell.val;

      if (idx > h) break;

      RingNode new_value_ring_node;
      if (!is_empty(val)) {
        if (idx == h) {
          new_value_ring_node.val = -1;
          new_value_ring_node.idx = (unsafe | h) + RING_SIZE;
          if (deq_cae(&rq->array[h & (RING_SIZE-1)].ring_node, &cell, &new_value_ring_node))
            return val;
        } else {
          new_value_ring_node.val = val;
          new_value_ring_node.idx = set_unsafe(idx);
          if (CAE(&rq->array[h & (RING_SIZE-1)].ring_node, &cell, &new_value_ring_node)) {
            break;
          }
        }
      } else {
        if ((r & ((1ull << 10) - 1)) == 0)
          tt = rq->tail;

        // Optimization: try to bail quickly if queue is closed.
        int crq_closed = crq_is_closed(tt);
        uint64_t t = tail_index(tt);

        if (unsafe) { // Nothing to do, move along
          new_value_ring_node.val = val;
          new_value_ring_node.idx = (unsafe | h) + RING_SIZE;
          if (CAE(&rq->array[h & (RING_SIZE-1)].ring_node, &cell, &new_value_ring_node))
            break;
        } else if (t < h + 1 || r > 200000 || crq_closed) {
          new_value_ring_node.val = val;
          new_value_ring_node.idx = h + RING_SIZE;
          //Do not believe this replaces starvation functionality
          if (CAE(&rq->array[h & (RING_SIZE-1)].ring_node, &cell, &new_value_ring_node)) {
            if (r > 200000 && tt > RING_SIZE)
              TAS_U64(&rq->tail, 63);
            break;
          }
        } else {
          ++r;
        }
      }
    }

    if (tail_index(rq->tail) <= h + 1) {
      //fixState(rq);
      // try to return empty
      next = rq->next;
      if (next == NULL)
        return -1;  // EMPTY
      if (tail_index(rq->tail) <= h + 1) {
        if (CAE(&q->head, &rq, &next)) {
          #if GC == 1
    				ssmem_free(alloc, (void*) rq);
    			#endif
    //      hzdptr_retire(&handle->hzdptr, rq);
        }
      }
    }
  }

  //hzdptr_clear(&handle->hzdptr, 0);
}

/*void queue_register(queue_t * q, handle_t * th, int id)
{
  th->next = NULL; // ADDED TO NOT BREAK EVERYTHING...
  hzdptr_init(&th->hzdptr, q->nprocs, 1);
}*/


void enqueue_(queue_t * q, handle_t * th, void * val)
{
  lcrq_put(q, th, (uint64_t) val);
}

void * dequeue_(queue_t * q, handle_t * th)
{
  return (void *) lcrq_get(q, th);
}
//By K
/*void handle_free(handle_t *h){
  hzdptr_t *hzd = &h->hzdptr;
  void **rlist = &hzd->ptrs[hzd->nptrs];
  for(int i = 0;i < hzd->nretired; i++){
    free(rlist[i]);
  }
  free(h->hzdptr.ptrs);
}*/
void queue_free(queue_t * q, handle_t * h){
  RingQueue *rq = q->head;
  while(rq){
    RingQueue *n = rq->next;
    free(rq);
    rq = n;
  };
}

// Wrappers which return bullshit values to fit into benchmarking framework
int enqueue_wrap(queue_t *q, handle_t *th, sval_t v) {
  enqueue_(q, th, (void*) v);
  return 1;
}

sval_t dequeue_wrap(queue_t *q, handle_t *th) {
  int64_t val = (int64_t) dequeue_(q, th);
  if (val != -1) return (sval_t) val;
  return 0;
}

//Need one more function here. Enq count does not guarantee uniqueness!
uint64_t lcrq_enq_count (queue_t *q){
  RingQueue *tail = q->tail;
  return tail_index(tail->tail) + tail->items_enqueued;
}

uint64_t lcrq_deq_count(queue_t *q){
  RingQueue *head = q->head;
  return head->head + head->items_enqueued;
}

uint64_t lcrq_queue_size(queue_t *q){
  uint64_t enq_count = lcrq_enq_count(q);
  uint64_t deq_count = lcrq_deq_count(q);

  if (enq_count <= deq_count) return 0;
  return enq_count - deq_count;
}

uint64_t lcrq_tail_version(queue_t *q){
  RingQueue *tail = q->tail;
  return (tail_index(tail->tail) & 0xFFFFFFFF) | (tail->items_enqueued << 32);
}
