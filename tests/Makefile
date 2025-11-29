# Makefile for floppy tests
# Run on host machine (not Pico)

CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -I../src
LDFLAGS =

# Source files
SRCDIR = ../src
SRCS = $(SRCDIR)/fat12.c $(SRCDIR)/lru.c $(SRCDIR)/mfm_decode.c $(SRCDIR)/mfm_encode.c $(SRCDIR)/f12.c

# All test binaries
TESTS = test_mfm test_fat12 test_lru test_robustness test_fuzz test_f12

.PHONY: all clean run

all: $(TESTS)

# Individual test targets
test_mfm: test_mfm.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_fat12: test_fat12.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_lru: test_lru.c $(SRCDIR)/lru.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_robustness: test_robustness.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_fuzz: test_fuzz.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_f12: test_f12.c $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Run all tests
run: all
	@echo ""
	@for t in $(TESTS); do \
		echo "========================================"; \
		./$$t || exit 1; \
	done
	@echo "========================================"
	@echo "All tests passed!"

clean:
	rm -f $(TESTS)
