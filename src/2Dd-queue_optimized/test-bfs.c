#include "graph.h"
#include <stdio.h>
#include "2Dd-queue_optimized.h"
#include "rapl_read.h"

char *filepath;
uint64_t root = 1;
bool directed = false;

size_t initial = DEFAULT_INITIAL;
size_t range = DEFAULT_RANGE;
size_t update = 100;
size_t load_factor;
size_t num_threads = DEFAULT_NB_THREADS;
size_t duration = DEFAULT_DURATION;

size_t print_vals_num = 100;
size_t pf_vals_num = 1023;
size_t put, put_explicit = false;
double update_rate, put_rate, get_rate;

size_t size_after = 0;
int seed = 0;
uint32_t rand_max;
#define rand_min 2

static volatile int stop;
uint64_t relaxation_bound = 1;
uint64_t width = 1;
uint64_t depth = 1;
uint64_t k_mode = 0;

TEST_VARS_GLOBAL;

volatile ticks *putting_succ;
volatile ticks *putting_fail;
volatile ticks *removing_succ;
volatile ticks *removing_fail;
volatile ticks *putting_count;
volatile ticks *putting_count_succ;
volatile unsigned long *put_cas_fail_count;
volatile unsigned long *get_cas_fail_count;
volatile unsigned long *null_count;
volatile unsigned long *hop_count;
volatile unsigned long *slide_count;
volatile ticks *removing_count;
volatile ticks *removing_count_succ;
volatile ticks *total;
volatile uint64_t active_threads;
uint64_t *start_times;
uint64_t *end_times;
uint64_t *work;
/* ################################################################### *
 * LOCALS
 * ################################################################### */

#ifdef DEBUG
extern __thread uint32_t put_num_restarts;
extern __thread uint32_t put_num_failed_expand;
extern __thread uint32_t put_num_failed_on_new;
#endif

__thread unsigned long *seeds;
__thread unsigned long my_put_cas_fail_count;
__thread unsigned long my_get_cas_fail_count;
__thread unsigned long my_null_count;
__thread unsigned long my_hop_count;
__thread unsigned long my_slide_count;
__thread int thread_id;

barrier_t barrier, barrier_global;

typedef struct thread_data
{
    uint32_t id;
    DS_TYPE *set;
    graph_t *g;
} thread_data_t;

#define MAX_FAILURES 100

uint64_t get_time()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec;
}

void run_bfs(DS_HANDLE set, graph_t *g)
{
    bool is_active = true;
    uint64_t failures = 0;
    while (failures < MAX_FAILURES || get_time() - end_times[thread_id] < 100000000 || active_threads != 0)
    {
        uint64_t current;
        while ((current = DS_REMOVE(set)))
        {
            // Successfully dequeued an item
            if (!is_active)
            {
                FAI_U64(&active_threads);
                is_active = true;
                failures = 0;
            }

            uint64_t *neighbors;
            uint64_t size = get_neighbors(g, current, &neighbors);
            uint64_t current_distance = g->distances[current];

            for (int i = 0; i < size; i++)
            {
                uint64_t current_neighbor = neighbors[i];
                uint64_t distance = g->distances[current_neighbor];
                uint64_t inc_current_distance = current_distance + 1;

                while (inc_current_distance < distance)
                {
                    if (likely(CAE(&g->distances[current_neighbor], &distance, &inc_current_distance)))
                    {
                        // Possible contention here. Could cache pad this array
                        work[thread_id]++;
                        DS_ADD(set, current_neighbor, current_neighbor);
                        break;
                    }
                }
            }
        }
        if (is_active)
        {
            FAD_U64(&active_threads);
            is_active = false;
            // Find the timestamp when the final thread did its first 'final' empty dequeue
            end_times[thread_id] = get_time();
        }
        failures += 1;
    }
}

void *test(void *thread)
{
    thread_data_t *td = (thread_data_t *)thread;
    thread_id = td->id;
    set_cpu(thread_id);
	seeds = seed_rand();

    THREAD_INIT(thread_id);
    PF_INIT(3, SSPFD_NUM_ENTRIES, thread_id);
#ifdef RELAXATION_TIMER_ANALYSIS
	if (thread_id == 0) init_relaxation_analysis_shared(num_threads);
    barrier_cross(&barrier);
	init_relaxation_analysis_local(thread_id);
#endif

    uint64_t my_putting_count = 0;
    uint64_t my_removing_count = 0;

    uint64_t my_putting_count_succ = 0;
    uint64_t my_removing_count_succ = 0;

    seeds = seed_rand();
    RR_INIT(thread_id);
    DS_HANDLE handle = DS_REGISTER(td->set, thread_id);

    if (thread_id == 0) DS_ADD(handle, root, root);
	td->g->distances[root] = 0;

    barrier_cross(&barrier);


    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    start_times[thread_id] = (uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec;

    run_bfs(handle, td->g);
    barrier_cross(&barrier_global);

    THREAD_END();
    pthread_exit(NULL);
}
int main(int argc, char **argv)
{
    set_cpu(0);
    seeds = seed_rand();

    struct option long_options[] = {
        // These options don't set a flag
        {"help", no_argument, NULL, 'h'},
        {"num-threads", required_argument, NULL, 'n'},
        {NULL, 0, NULL, 0}};

    int i, c;
    while (1)
    {
        i = 0;
        c = getopt_long(argc, argv, "hAf:di:n:r:u:m:a:l:p:b:v:f:y:z:k:w:s:c:", long_options, &i);
        if (c == -1)
            break;
        if (c == 0 && long_options[i].flag == 0)
            c = long_options[i].val;
        switch (c)
        {
        case 0:
            /* Flag is automatically set */
            break;
        case 'h':
            printf("BFS"
                   "\n"
                   "\n"
                   "Usage:\n"
                   "  %s [options...]\n"
                   "\n"
                   "Options:\n"
                   "  -h, --help\n"
                   "        Print this message\n"
                   "  -n, --num-threads <int>\n"
                   "        Number of threads\n"
                   "  -k, --Relaxation-bound <int>\n"
                   "        Relaxation bound.\n"
                   "  -l, --Depth <int>\n"
                   "        Locality/Depth if k-mode is set to zero.\n"
                   "  -w, --Width <int>\n"
                   "        Fixed Width or Width to thread ratio depending on the k-mode.\n"
                   "  -m, --K Mode <int>\n"
                   "        0 for Fixed Width and Depth, 1 for Fixed Width, 2 for fixed Depth, 3 for fixed Width to thread ratio.\n"
                   "  -f, --filepath <str>\n"
                   "        The filepath to the .mtx file.\n"
                   "  -r, --root <int>\n"
                   "        The starting node of the bfs.\n"
                   "  -d, --directed \n"
                   "        Parses the graph as directed [DEFAULT=false].\n",
                   argv[0]);
            exit(0);
        case 'n':
            num_threads = atoi(optarg);
            break;
        case 'f':
            filepath = optarg;
            break;
        case 'r':
            root = atoi(optarg);
            break;
        case 'd':
            directed = true;
            break;
        case 'k':
            if (atoi(optarg) > 0)
                relaxation_bound = atoi(optarg);
            break;
        case 'l':
            if (atoi(optarg) > 0)
                depth = atoi(optarg);
            break;
        case 'w':
            if (atoi(optarg) > 0)
                width = atoi(optarg);
            break;
        case 'm':
            if (atoi(optarg) <= 3)
                k_mode = atoi(optarg);
            break;
        case 'c':
            break;
        case '?':
        default:
            printf("Use -h or --help for help\n");
            exit(1);
        }
    }

    struct timeval start, end;
    struct timespec timeout;
    timeout.tv_sec = duration / 1000;
    timeout.tv_nsec = (duration % 1000) * 1000000;
    stop = 0;

	DS_TYPE* set = DS_NEW(num_threads, width, depth, k_mode, relaxation_bound, num_threads);
    assert(set != NULL);

    /* Initializes the local data */
    putting_succ = (ticks *)calloc(num_threads, sizeof(ticks));
    putting_fail = (ticks *)calloc(num_threads, sizeof(ticks));
    removing_succ = (ticks *)calloc(num_threads, sizeof(ticks));
    removing_fail = (ticks *)calloc(num_threads, sizeof(ticks));
    putting_count = (ticks *)calloc(num_threads, sizeof(ticks));
    putting_count_succ = (ticks *)calloc(num_threads, sizeof(ticks));
    removing_count = (ticks *)calloc(num_threads, sizeof(ticks));
    removing_count_succ = (ticks *)calloc(num_threads, sizeof(ticks));
    put_cas_fail_count = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    get_cas_fail_count = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    null_count = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    slide_count = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    hop_count = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    start_times = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    end_times = (unsigned long *)calloc(num_threads, sizeof(unsigned long));
    work = (unsigned long *)calloc(num_threads, sizeof(unsigned long));

    pthread_t threads[num_threads];
    pthread_attr_t attr;
    int rc;
    void *status;

    // ad initialize barriers
    barrier_init(&barrier_global, num_threads + 1);
    barrier_init(&barrier, num_threads);

    /* Initialize and set thread detached attribute */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    graph_t *g = parse_mtx_file(filepath, directed);

    thread_data_t *tds = (thread_data_t *)malloc(num_threads * sizeof(thread_data_t));

    active_threads = num_threads;

    long t;
    for (t = 0; t < num_threads; t++)
    {
        tds[t].id = t;
        tds[t].set = set;
        tds[t].g = g;
        rc = pthread_create(&threads[t], &attr, test, tds + t); // ad create thread and call test function
        if (rc)
        {
            printf("ERROR; return code from pthread_create() is %d\n", rc);
            exit(-1);
        }
    }

    /* Free attribute and wait for the other threads */
    pthread_attr_destroy(&attr);
    /*main thread will wait on the &barrier_global until all threads within test have reached
    and set the timer before they cross to start the test loop*/
    barrier_cross(&barrier_global);

    gettimeofday(&start, NULL);
    nanosleep(&timeout, NULL);

    stop = 1;
    gettimeofday(&end, NULL);
    duration = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);

    for (t = 0; t < num_threads; t++)
    {
        rc = pthread_join(threads[t], &status);
        if (rc)
        {
            printf("ERROR; return code from pthread_join() is %d\n", rc);
            exit(-1);
        }
    }

    free(tds);

    uint64_t min_start = start_times[0];
    uint64_t max_end = end_times[0];
    uint64_t total_work = 0;

    for (uint64_t i = 0; i < num_threads; i++)
    {
        uint64_t start_time = start_times[i];
        uint64_t end_time = end_times[i];

        if (start_time < min_start)
        {
            min_start = start_time;
        }

        if (end_time > max_end)
        {
            max_end = end_time;
        }
        total_work += work[i];
    }

    uint64_t distances = 0;
    uint64_t visited = 0;
    for(uint64_t i = 1; i <= g->n_verticies; i++) {
		uint64_t distance = g->distances[i];
		if (distance != UINT64_MAX){
			visited++;
			distances += distance;
		}
	}

	// Print graph metrics
	printf("elapsed_time , %.3f \n", ((double)max_end - min_start)/1000000);
	printf("average_distance , %.3f \n", ((double)distances/visited));
	printf("vertices_visited , %lu \n", visited);
	printf("total_work , %lu \n", total_work);


    volatile ticks putting_suc_total = 0;
    volatile ticks putting_fal_total = 0;
    volatile ticks removing_suc_total = 0;
    volatile ticks removing_fal_total = 0;
    volatile uint64_t putting_count_total = 0;
    volatile uint64_t putting_count_total_succ = 0;
    volatile unsigned long put_cas_fail_count_total = 0;
    volatile unsigned long get_cas_fail_count_total = 0;
    volatile unsigned long null_count_total = 0;
    volatile unsigned long slide_count_total = 0;
    volatile unsigned long hop_count_total = 0;
    volatile uint64_t removing_count_total = 0;
    volatile uint64_t removing_count_total_succ = 0;

    for (t = 0; t < num_threads; t++)
    {
        PRINT_OPS_PER_THREAD();
        putting_suc_total += putting_succ[t];
        putting_fal_total += putting_fail[t];
        removing_suc_total += removing_succ[t];
        removing_fal_total += removing_fail[t];
        putting_count_total += putting_count[t];
        putting_count_total_succ += putting_count_succ[t];
        put_cas_fail_count_total += put_cas_fail_count[t];
        get_cas_fail_count_total += get_cas_fail_count[t];
        null_count_total += null_count[t];
        hop_count_total += hop_count[t];
        slide_count_total += slide_count[t];
        removing_count_total += removing_count[t];
        removing_count_total_succ += removing_count_succ[t];
    }

#if defined(COMPUTE_LATENCY)
    printf("#thread srch_suc srch_fal insr_suc insr_fal remv_suc remv_fal   ## latency (in cycles) \n");
    fflush(stdout);
    long unsigned put_suc = putting_count_total_succ ? putting_suc_total / putting_count_total_succ : 0;
    long unsigned put_fal = (putting_count_total - putting_count_total_succ) ? putting_fal_total / (putting_count_total - putting_count_total_succ) : 0;
    long unsigned rem_suc = removing_count_total_succ ? removing_suc_total / removing_count_total_succ : 0;
    long unsigned rem_fal = (removing_count_total - removing_count_total_succ) ? removing_fal_total / (removing_count_total - removing_count_total_succ) : 0;
    printf("%-7zu %-8lu %-8lu %-8lu %-8lu %-8lu %-8lu\n", num_threads, get_suc, get_fal, put_suc, put_fal, rem_suc, rem_fal);
#endif

#define LLU long long unsigned int

    int UNUSED pr = (int)(putting_count_total_succ - removing_count_total_succ);
    uint64_t total = putting_count_total + removing_count_total;
    double putting_perc = 100.0 * (1 - ((double)(total - putting_count_total) / total));
    double putting_perc_succ = (1 - (double)(putting_count_total - putting_count_total_succ) / putting_count_total) * 100;
    double removing_perc = 100.0 * (1 - ((double)(total - removing_count_total) / total));
    double removing_perc_succ = (1 - (double)(removing_count_total - removing_count_total_succ) / removing_count_total) * 100;

    printf("putting_count_total , %-10llu \n", (LLU)putting_count_total);
    printf("putting_count_total_succ , %-10llu \n", (LLU)putting_count_total_succ);
    printf("putting_perc_succ , %10.1f \n", putting_perc_succ);
    printf("putting_perc , %10.1f \n", putting_perc);
    printf("putting_effective , %10.1f \n", (putting_perc * putting_perc_succ) / 100);

    printf("removing_count_total , %-10llu \n", (LLU)removing_count_total);
    printf("removing_count_total_succ , %-10llu \n", (LLU)removing_count_total_succ);
    printf("removing_perc_succ , %10.1f \n", removing_perc_succ);
    printf("removing_perc , %10.1f \n", removing_perc);
    printf("removing_effective , %10.1f \n", (removing_perc * removing_perc_succ) / 100);

    double throughput = (putting_count_total + removing_count_total) * 1000.0 / (max_end - min_start);

    printf("num_threads , %zu \n", num_threads);
    printf("Mops , %.3f\n", throughput / 1e6);
    //	printf("Ops , %.2f\n", throughput);

    RR_PRINT_CORRECTED();
    RETRY_STATS_PRINT(total, putting_count_total, removing_count_total, putting_count_total_succ + removing_count_total_succ);
    LATENCY_DISTRIBUTION_PRINT();

#ifdef RELAXATION_TIMER_ANALYSIS
    print_relaxation_measurements(num_threads);
#elif RELAXATION_ANALYSIS
    print_relaxation_measurements();
#else
    printf("Push_CAS_fails , %zu\n", put_cas_fail_count_total);
    printf("Pop_CAS_fails , %zu\n", get_cas_fail_count_total);
#endif
    printf("Null_Count , %zu\n", null_count_total);
    printf("Hop_Count , %zu\n", hop_count_total);
    printf("Slide_Count , %zu\n", slide_count_total);

    pthread_exit(NULL);

    return 0;
}