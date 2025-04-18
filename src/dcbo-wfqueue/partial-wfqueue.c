#include "partial-wfqueue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "primitives.h"

#ifdef RELAXATION_TIMER_ANALYSIS
#include "relaxation_analysis_timestamps.c"
__thread uint64_t enq_timestamp, deq_timestamp;
#define ENQ_TIMESTAMP (enq_timestamp = get_timestamp())
#define DEQ_TIMESTAMP (deq_timestamp = get_timestamp())
#elif RELAXATION_ANALYSIS
#error "Cannot use lock-based relaxation analysis for wfqueue due to complexity of helping threads"
#else
#define ENQ_TIMESTAMP
#define DEQ_TIMESTAMP
#endif

#define N WFQUEUE_NODE_SIZE
#define BOT ((void *)0)
#define TOP ((void *)-1)

#define MAX_GARBAGE(n) (2 * n)

#ifndef MAX_SPIN
#define MAX_SPIN 100
#endif

#ifndef MAX_PATIENCE
#define MAX_PATIENCE 10
#endif

typedef struct _enq_t enq_t;
typedef struct _deq_t deq_t;
typedef struct _cell_t cell_t;
typedef struct _node_t node_t;


// Spins until the value v is set to something
static inline void *spin(void *volatile *p) {
    int patience = MAX_SPIN;
    void *v = *p;

    while (!v && patience-- > 0) {
        v = *p;
        PAUSE();
    }

    return v;
}

static inline node_t *new_node() {
	#if GC == 1
        // node_t *n = align_malloc(PAGE_SIZE, sizeof(node_t));
        node_t *n = ssmem_alloc(alloc, sizeof(node_t));
	#else
	  	node_t* n = malloc(sizeof(node_t));
	#endif
    memset(n, 0, sizeof(node_t));
    return n;
}

static node_t *check(unsigned long volatile *p_hzd_node_id, node_t *cur,
                     node_t *old) {
    unsigned long hzd_node_id = ACQUIRE(p_hzd_node_id);

    if (hzd_node_id < cur->id) {
        node_t *tmp = old;
        while (tmp->id < hzd_node_id) {
            tmp = tmp->next;
        }
        cur = tmp;
    }

    return cur;
}

static node_t *update(node_t *volatile *pPn, node_t *cur,
                      unsigned long volatile *p_hzd_node_id, node_t *old) {
    node_t *ptr = ACQUIRE(pPn);

    if (ptr->id < cur->id) {
        if (!CAScs(pPn, &ptr, cur)) {
            if (ptr->id < cur->id) cur = ptr;
        }

        cur = check(p_hzd_node_id, cur, old);
    }

    return cur;
}

static void cleanup(queue_t *q, handle_t *th) {
    long oid = ACQUIRE(&q->Hi);
    node_t *new = th->Dp;

    if (oid == -1) return;
    if (new->id - oid < MAX_GARBAGE(q->nprocs)) return;
    if (!CASa(&q->Hi, &oid, -1)) return;

    long Di = q->Di, Ei = q->Ei;
    while(Ei <= Di && !CAS(&q->Ei, &Ei, Di + 1))
        ;

    node_t *old = q->Hp;
    handle_t *ph = th;
    handle_t *phs[q->nprocs];
    assert(q->nprocs > 0);
    int i = 0;

    do {
        new = check(&ph->hzd_node_id, new, old);
        new = update(&ph->Ep, new, &ph->hzd_node_id, old);
        new = update(&ph->Dp, new, &ph->hzd_node_id, old);

        assert(i < q->nprocs); // ERROR! Does not seem like the handles are linked together properly
        phs[i++] = ph;
        ph = ph->next;
    } while (new->id > oid && ph != th);

    while (new->id > oid && --i >= 0) {
        new = check(&phs[i]->hzd_node_id, new, old);
    }

    long nid = new->id;

    if (nid <= oid) {
        RELEASE(&q->Hi, oid);
    } else {
        q->Hp = new;
        RELEASE(&q->Hi, nid);

        while (old != new) {
            node_t *tmp = old->next;
			#if GC == 1
                // free(old);
				ssmem_free(alloc, (void*) old);
			#endif
            old = tmp;
        }
    }
}

void add_relaxed_enq(void* val)
{
#ifdef RELAXATION_TIMER_ANALYSIS
	add_relaxed_put((sval_t) val, enq_timestamp);
#endif
}

void add_relaxed_deq(void* val)
{
#ifdef RELAXATION_TIMER_ANALYSIS
	add_relaxed_get((sval_t) val, deq_timestamp);
#endif
}

static cell_t *find_cell(node_t *volatile *ptr, long i, handle_t *th) {
    node_t *curr = *ptr;

    long j;
    for (j = curr->id; j < i / N; ++j) {
        node_t *next = curr->next;

        if (next == NULL) {
            node_t *temp = th->spare;

            if (!temp) {
                temp = new_node();
                th->spare = temp;
            }

            temp->id = j + 1;

            if (CASra(&curr->next, &next, temp)) {
                next = temp;
                th->spare = NULL;
            }
        }

        curr = next;
    }

    *ptr = curr;
    return &curr->cells[i % N];
}

static int enq_fast(queue_t *q, handle_t *th, void *v, long *id) {
    long i = FAAcs(&q->Ei, 1);
    ENQ_TIMESTAMP;
    void *cv = BOT;
    cell_t *c = find_cell(&th->Ep, i, th);

    if (CAS(&c->val, &cv, v)) {
#ifdef RECORD
        th->fastenq++;
#endif
        add_relaxed_enq(v);
        return 1;
    } else {
        *id = i;
        return 0;
    }
}

static void enq_slow(queue_t *q, handle_t *th, void *v, long id) {
    enq_t *enq = &th->Er;
    enq->val = v;
    RELEASE(&enq->id, id);

    node_t *tail = th->Ep;
    long i;
    cell_t *c;

    // Claim a cell and publish the enqueue request
    do {
        i = FAA(&q->Ei, 1);
        ENQ_TIMESTAMP;
        c = find_cell(&tail, i, th);
        enq_t *ce = BOT;

        if (CAScs(&c->enq, &ce, enq) && c->val != TOP) {
            if (CAS(&enq->id, &id, -i)) id = -i;
            break;
        }
    } while (enq->id > 0);

    id = -enq->id;
    c = find_cell(&th->Ep, id, th);
    if (id > i) {
        long Ei = q->Ei;
        while (Ei <= id && !CAS(&q->Ei, &Ei, id + 1))
            ;
    }

    add_relaxed_enq(v);
    c->val = v;

#ifdef RECORD
    th->slowenq++;
#endif
}

void wfqueue_enqueue(queue_t *q, handle_t *th, void *v) {
    th->hzd_node_id = th->enq_node_id;

    long id;
    int p = MAX_PATIENCE;
    while (!enq_fast(q, th, v, &id) && p-- > 0)
        ;
    if (p < 0) enq_slow(q, th, v, id);

    th->enq_node_id = th->Ep->id;
    RELEASE(&th->hzd_node_id, -1);
}

static void *help_enq(queue_t *q, handle_t *th, cell_t *c, long i) {
    void *v = spin(&c->val);

    if ((v != TOP && v != BOT) ||
        (v == BOT && !CAScs(&c->val, &v, TOP) && v != TOP)) {
        return v;
    }

    enq_t *e = c->enq;

    if (e == BOT) {
        handle_t *ph;
        enq_t *pe;
        long id;
        ph = th->Eh, pe = &ph->Er, id = pe->id;

        if (th->Ei != 0 && th->Ei != id) {
            th->Ei = 0;
            th->Eh = ph->next;
            ph = th->Eh, pe = &ph->Er, id = pe->id;
        }

        if (id > 0 && id <= i && !CAS(&c->enq, &e, pe) && e != pe)
            th->Ei = id;
        else {
            th->Ei = 0;
            th->Eh = ph->next;
        }

        if (e == BOT && CAS(&c->enq, &e, TOP)) e = TOP;
    }

    if (e == TOP) return (q->Ei <= i ? BOT : TOP);

    long ei = ACQUIRE(&e->id);
    void *ev = ACQUIRE(&e->val);

    if (ei > i) {
        if (c->val == TOP && q->Ei <= i) return BOT;
    } else {
        if ((ei > 0 && CAS(&e->id, &ei, -i)) || (ei == -i && c->val == TOP)) {
            long Ei = q->Ei;
            while (Ei <= i && !CAS(&q->Ei, &Ei, i + 1))
                ;
            c->val = ev;
        }
    }

    return c->val;
}

static void help_deq(queue_t *q, handle_t *th, handle_t *ph) {
    deq_t *deq = &ph->Dr;
    long idx = ACQUIRE(&deq->idx);
    long id = deq->id;

    if (idx < id) return;

    node_t *Dp = ph->Dp;
    th->hzd_node_id = ph->hzd_node_id;
    FENCE();
    idx = deq->idx;

    long i = id + 1, old = id, new = 0;
    while (1) {
        DEQ_TIMESTAMP; // This could be moved to a more accurate place
        node_t *h = Dp;
        for (; idx == old && new == 0; ++i) {
            cell_t *c = find_cell(&h, i, th);

            long Di = q->Di;
            while (Di <= i && !CAS(&q->Di, &Di, i + 1))
                ;

            void *v = help_enq(q, th, c, i);
            if (v == BOT || (v != TOP && c->deq == BOT))
                new = i;
            else
                idx = ACQUIRE(&deq->idx);
        }

        if (new != 0) {
            if (CASra(&deq->idx, &idx, new)) idx = new;
            if (idx >= new) new = 0;
        }

        if (idx < 0 || deq->id != id) break;

        cell_t *c = find_cell(&Dp, idx, th);
        deq_t *cd = BOT;
        if (c->val == TOP || CAS(&c->deq, &cd, deq) || cd == deq) {
            CAS(&deq->idx, &idx, -idx);
            break;
        }

        old = idx;
        if (idx >= i) i = idx + 1;
    }
}

static void *deq_fast(queue_t *q, handle_t *th, long *id) {
    // Early Empty return
    long Di = (long) q->Di;
    long Ei = (long) q->Ei;
    if (Di >= Ei) return INTERNAL_EMPTY;

    long i = FAAcs(&q->Di, 1);
    DEQ_TIMESTAMP;
    cell_t *c = find_cell(&th->Dp, i, th);
    void *v = help_enq(q, th, c, i);
    deq_t *cd = BOT;

    if (v == BOT) return BOT; // Empty return
    if (v != TOP && CAS(&c->deq, &cd, TOP))
    {
        add_relaxed_deq(v);
        return v;
    }

    *id = i;
    return TOP;
}

static void *deq_slow(queue_t *q, handle_t *th, long id) {
    deq_t *deq = &th->Dr;
    RELEASE(&deq->id, id);
    RELEASE(&deq->idx, id);

    DEQ_TIMESTAMP;
    help_deq(q, th, th);
    long i = -deq->idx;
    cell_t *c = find_cell(&th->Dp, i, th);
    void *val = c->val;

    if (val != TOP && val != BOT) add_relaxed_deq(val);

#ifdef RECORD
    th->slowdeq++;
#endif
    return val == TOP ? BOT : val;
}

void *wfqueue_dequeue(queue_t *q, handle_t *th) {
    th->hzd_node_id = th->deq_node_id;

    void *v;
    long id = 0;
    int p = MAX_PATIENCE;

    do
        v = deq_fast(q, th, &id);
    while (v == TOP && p-- > 0);
    if (v == TOP)
        v = deq_slow(q, th, id);
    else {
#ifdef RECORD
        th->fastdeq++;
#endif
    }

    if (v != INTERNAL_EMPTY) {
        help_deq(q, th, th->Dh);
        th->Dh = th->Dh->next;
    }

    th->deq_node_id = th->Dp->id;
    RELEASE(&th->hzd_node_id, -1);

    if (th->spare == NULL) {
        cleanup(q, th);
        th->spare = new_node();
    }

#ifdef RECORD
    if (v == INTERNAL_EMPTY) th->empty++;
#endif
    return v;
}

// Also init SSMEM if not allocated
void wfqueue_init(queue_t *q, int nprocs) {
    q->Hi = 0;
    q->Hp = new_node();

    q->Ei = 1;
    q->Di = 1;

    assert(nprocs != 0);
    q->nprocs = nprocs;

#ifdef RECORD
    q->fastenq = 0;
    q->slowenq = 0;
    q->fastdeq = 0;
    q->slowdeq = 0;
    q->empty = 0;
#endif
    // pthread_barrier_init(&barrier, NULL, nprocs);
}

queue_t* wfqueue_create(int nprocs, int thread_id)
{
    queue_t* wfqueue = aligned_alloc(CACHE_LINE_SIZE, sizeof(queue_t));
    wfqueue_init(wfqueue, nprocs);
    return wfqueue;
}

void queue_free(queue_t *q, handle_t *h) {
#ifdef RECORD
    static int lock = 0;

    FAA(&q->fastenq, h->fastenq);
    FAA(&q->slowenq, h->slowenq);
    FAA(&q->fastdeq, h->fastdeq);
    FAA(&q->slowdeq, h->slowdeq);
    FAA(&q->empty, h->empty);

    // pthread_barrier_wait(&barrier);

    if (FAA(&lock, 1) == 0)
        printf("Enq: %f Deq: %f Empty: %f\n",
               q->slowenq * 100.0 / (q->fastenq + q->slowenq),
               q->slowdeq * 100.0 / (q->fastdeq + q->slowdeq),
               q->empty * 100.0 / (q->fastdeq + q->slowdeq));
#endif
}

// Inits a handle. Modified from normal to fit a partial queue
handle_t* wfqueue_register(queue_t *q, handle_t *th, int id) {

    th->queue=q;

    th->next = NULL;
    th->hzd_node_id = -1;
    th->Ep = q->Hp;
    th->enq_node_id = th->Ep->id;
    th->Dp = q->Hp;
    th->deq_node_id = th->Dp->id;

    th->Er.id = 0;
    th->Er.val = BOT;
    th->Dr.id = 0;
    th->Dr.idx = -1;

    th->Ei = 0;
    th->spare = new_node();
#ifdef RECORD
    th->slowenq = 0;
    th->slowdeq = 0;
    th->fastenq = 0;
    th->fastdeq = 0;
    th->empty = 0;
#endif

    handle_t *tail = q->_tail_handle;

    if (tail == NULL) {
        th->next = th;
        if (CASra(&q->_tail_handle, &tail, th)) {
            th->Eh = th->next;
            th->Dh = th->next;
            return th;
        }
    }

    handle_t *next = tail->next;
    do
        th->next = next;
    while (!CASra(&tail->next, &next, th));

    th->Eh = th->next;
    th->Dh = th->next;

    return th;
}

// Wrappers which return bullshit values to fit into benchmarking framework
int enqueue_wrap(handle_t *th, void *v) {
  wfqueue_enqueue(th->queue, th, v);
  return 1;
}

sval_t dequeue_wrap(handle_t *th) {
  return (sval_t)wfqueue_dequeue(th->queue, th);
}

uint64_t wfqueue_enq_count(queue_t *q)
{
    return q->Ei;
}

uint64_t wfqueue_deq_count(queue_t *q)
{
    return q->Di;
}

uint64_t wfqueue_length_heuristic(queue_t *q)
{
    uint64_t Ei = q->Ei;
    uint64_t Di = q->Di;

    if (Ei < Di) return 0;
    return Ei - Di;
}

