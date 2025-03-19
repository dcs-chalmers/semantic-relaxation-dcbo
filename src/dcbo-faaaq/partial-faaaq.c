#include "partial-faaaq.h"

#ifdef RELAXATION_ANALYSIS
#include "relaxation_analysis_queue.c"
#elif RELAXATION_TIMER_ANALYSIS
#include "relaxation_analysis_timestamps.c"
#endif

// Want timers at FAA increments and not with the normal CAE
#ifdef RELAXATION_TIMER_ANALYSIS
__thread uint64_t enq_timestamp, deq_timestamp;
#define ENQ_TIMESTAMP (enq_timestamp = get_timestamp())
#define DEQ_TIMESTAMP (deq_timestamp = get_timestamp())
#else
#define ENQ_TIMESTAMP
#define DEQ_TIMESTAMP
#endif


// TODO: use ssmem_alloc when using GC (this uses ssmem instead of ssalloc)
segment_t* create_segment(skey_t key, sval_t val, segment_t* next, uint64_t node_idx) {
	#if GC == 1
        segment_t* segment = (segment_t*) ssmem_alloc(alloc, sizeof(segment_t) + BUFFER_SIZE*sizeof(sval_t));
	#else
        segment_t* segment = (segment_t*) ssalloc(sizeof(segment_t) + BUFFER_SIZE*sizeof(sval_t));
	#endif
    segment->next = NULL;
    segment->deq_idx = 0;
    segment->enq_idx = 1;
    segment->node_idx = node_idx;

    segment->items[0] = val;
    memset((void*) &segment->items[1], 0, (BUFFER_SIZE - 1)*sizeof(sval_t));
    return segment;
}

static int enq_cae(volatile sval_t* item_loc, sval_t new_value)
{
	sval_t expected = EMPTY;
#ifdef RELAXATION_TIMER_ANALYSIS
	// Use timers to track relaxation instead of locks
	if (CAE(item_loc, &expected, &new_value))
	{
		// Save this count in a local array of (timestamp, )
		add_relaxed_put(new_value, enq_timestamp);
		return true;
	}
	return false;
#elif RELAXATION_ANALYSIS
	lock_relaxation_lists();

	if (CAE(item_loc, &expected, &new_value))
	{
        *item_loc = gen_relaxation_count();
		add_linear(*item_loc, 0);
		unlock_relaxation_lists();
		return true;
	}
	else {
		unlock_relaxation_lists();
		return false;
	}
#else
	return CAE(item_loc, &expected, &new_value);
#endif
}

static sval_t deq_swp(volatile sval_t* item_loc)
{
#ifdef RELAXATION_TIMER_ANALYSIS
	// Use timers to track relaxation instead of locks
    sval_t item = SWAP_U64(item_loc, TAKEN);
	if (item != EMPTY)
	{
		add_relaxed_get(item, deq_timestamp);
	}
	return item;
#elif RELAXATION_ANALYSIS
	lock_relaxation_lists();
    sval_t item = SWAP_U64(item_loc, TAKEN);
	if (item != EMPTY)
	{
		remove_linear(item);
	}
	unlock_relaxation_lists();
	return item;
#else
    return SWAP_U64(item_loc, TAKEN);
#endif
}

int faaaq_enqueue(faaaq_t *q, skey_t key, sval_t val){
    while (true)
    {
        segment_t *tail = q->tail;
        //Linearization point
        uint64_t idx = FAI_U64(&tail->enq_idx);
        ENQ_TIMESTAMP;
        if(idx > BUFFER_SIZE - 1)
        {
            if (tail != q->tail) continue;
            segment_t *next = tail->next;
            if(next == NULL)
            {
                //Create segment (node)
                segment_t *new_segment = create_segment(key, val, NULL, tail->node_idx + 1);
                segment_t* null_segment = NULL;
                if(CAE(&tail->next, &null_segment, &new_segment)){
                    CAE(&q->tail, &tail, &new_segment);
                    #ifdef RELAXATION_TIMER_ANALYSIS
		                add_relaxed_put(val, enq_timestamp);
                    #endif

                    return 1;
                }
                #if GC == 1
					ssmem_free(alloc, (void*) new_segment);
				#endif

            }
            else {
                CAE(&q->tail, &tail, &next);
            }
            continue;
        }
        if (enq_cae(&tail->items[idx], val))
        {
            return 1;
        }
    }
}

sval_t faaaq_dequeue(faaaq_t *q) {
    while (true)
    {
        segment_t *head = q->head;
        if (head->deq_idx >= head->enq_idx && head->next == NULL) break;
        //Linearization point
        uint64_t idx = FAI_U64(&head->deq_idx);
        DEQ_TIMESTAMP;
        if(idx > BUFFER_SIZE - 1)
        {
            segment_t *next = head->next;
            if(next == NULL) break;
            if (CAE(&q->head, &head, &next))
            {
                #if GC == 1
    				ssmem_free(alloc, (void*) head);
    			#endif
            }
            continue;

        }
        sval_t item = deq_swp(&head->items[idx]);
        if(item != EMPTY)
        {
            return item;
        }
    }
    return 0;
}

void init_faaaq_queue(faaaq_t *q) {
	#if GC == 1
        segment_t* segment = (segment_t*) ssmem_alloc(alloc, sizeof(segment_t) + BUFFER_SIZE*sizeof(sval_t));
	#else
        segment_t* segment = (segment_t*) ssalloc(sizeof(segment_t) + BUFFER_SIZE*sizeof(sval_t));
	#endif
    segment->next = NULL;
    segment->deq_idx = 0;
    segment->enq_idx = 0;
    segment->node_idx = 0;
    // Fill first slot with "sentinel"
    memset((void*) &segment->items[0], 0, BUFFER_SIZE*sizeof(sval_t));

	q->head = segment;
	q->tail = segment;
}

size_t faaaq_queue_size(faaaq_t *q)
{
    uint64_t enq_count = faaaq_enq_count(q);
    uint64_t deq_count = faaaq_deq_count(q);

    if (enq_count < deq_count) return 0;
    return enq_count - deq_count;
}

uint64_t faaaq_enq_count(faaaq_t *q)
{
    segment_t* tail = q->tail;
    uint64_t idx = tail->enq_idx;
    if(idx > BUFFER_SIZE - 1) idx = BUFFER_SIZE;
    return idx + BUFFER_SIZE * tail->node_idx;
}

uint64_t faaaq_deq_count(faaaq_t *q)
{
    segment_t* head = q->head;
    uint64_t idx = head->deq_idx;
    if(idx > BUFFER_SIZE - 1) idx = BUFFER_SIZE;
    return idx + BUFFER_SIZE * head->node_idx;
}
