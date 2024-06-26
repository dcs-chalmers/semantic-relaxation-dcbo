# Data structure description

The WFQ Simple d-CBO (d-Choice Balanced Operations) queue uses the choice of d to balance enqueue and dequeue counts across several sub-queues. By compiling with `HEURISTIC=LENGTH`, you instead get the d-CBL, which balances sub-queue lengths instead of operation counts. The Simple d-CBO uses external and exact counters for operation counts. The WFQ is similar to the LCRQ, but achieves wait-freedom by sacrificing the circular arrays, also adding helping functionalities, and is used as the sub-queue here.

## Origin

To from the paper _Balanced Allocations over Efficient Queues: A Fast Relaxed FIFO Queue_, to be published in PPoPP 2025.

## Main Author

Kåre von Geijer <karev@chalmers.se>