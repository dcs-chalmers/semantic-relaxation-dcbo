ROOT = ../..

include $(ROOT)/common/Makefile.common

ALGO_O = stack-s_relaxed-k-segment.o
ALGO_C = stack-s_relaxed-k-segment.c

BINS = "$(BINDIR)/stack-k-segment"
PROF = $(ROOT)/src

.PHONY:	all clean

all:	main

measurements.o:
	$(CC) $(CFLAGS) -c -o $(BUILDIR)/measurements.o $(PROF)/measurements.c

ssalloc.o:
	$(CC) $(CFLAGS) -c -o $(BUILDIR)/ssalloc.o $(PROF)/ssalloc.c

stack-lockfree.o:
	$(CC) $(CFLAGS) -c -o $(BUILDIR)/stack-lockfree.o stack-lockfree.c

$(ALGO_O): stack-lockfree.h
	$(CC) $(CFLAGS) -c -o $(BUILDIR)/$(ALGO_O) $(ALGO_C)

intset.o: stack-lockfree.h stack-s_relaxed-k-segment.h
	$(CC) $(CFLAGS) -c -o $(BUILDIR)/intset.o intset.c

test.o: stack-lockfree.h stack-s_relaxed-k-segment.h intset.h
	$(CC) $(CFLAGS) -c -o $(BUILDIR)/test.o $(TEST_FILE)

main: measurements.o ssalloc.o stack-lockfree.o $(ALGO_O) intset.o test.o
	$(CC) $(CFLAGS) $(BUILDIR)/measurements.o $(BUILDIR)/ssalloc.o $(BUILDIR)/stack-lockfree.o $(BUILDIR)/$(ALGO_O) $(BUILDIR)/intset.o $(BUILDIR)/test.o -o $(BINS) $(LDFLAGS)

clean:
	-rm -f $(BINS) *.o
