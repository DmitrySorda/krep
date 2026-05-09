# krep - A high-performance string search utility
# Author: Davide Santangelo
# Version: 2.3.0

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

CC = gcc
CFLAGS = -Wall -Wextra -O3 -std=c11 -pthread -D_GNU_SOURCE -D_DEFAULT_SOURCE \
         -flto -funroll-loops -finline-functions
LDFLAGS = -pthread -flto

# Build mode: set NATIVE=1 for local tuning only. SIMD code paths are compiled
# with per-function target attributes and selected at runtime.
# Example: make NATIVE=1
ifdef NATIVE
    CFLAGS += -mtune=native
endif

# PCRE2 JIT support: set PCRE2=1 to link against libpcre2-8 and enable the
# JIT-accelerated regex engine.  Requires libpcre2-dev / brew install pcre2.
# Example: make PCRE2=1
ifdef PCRE2
    PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8 2>/dev/null || echo "-I/usr/local/include")
    PCRE2_LIBS   := $(shell pkg-config --libs   libpcre2-8 2>/dev/null || echo "-L/usr/local/lib -lpcre2-8")
    CFLAGS  += -DHAVE_PCRE2 $(PCRE2_CFLAGS)
    LDFLAGS += $(PCRE2_LIBS)
endif

# The binary intentionally avoids compile-host SIMD flags such as -mavx2.
# Runtime dispatch in krep.c selects AVX-512/AVX2/SSE2/NEON when available.

# Source files
SRCS = krep.c aho_corasick.c
LMDB_OBJS =

# LMDB trigram index: set LMDB=1 to compile in the krep_index layer.
# Uses the bundled LMDB source in lmdb/ (no external dependency required).
# Example: make LMDB=1
ifdef LMDB
    CFLAGS    += -DHAVE_LMDB -Ilmdb
    LMDB_OBJS  = lmdb/mdb.o lmdb/midl.o krep_index.o
endif

OBJS = $(SRCS:.c=.o) $(LMDB_OBJS)

# Test source files
TEST_SRCS = test/test_krep.c test/test_regex.c test/test_multiple_patterns.c
TEST_OBJS_MAIN = krep_test.o aho_corasick_test.o # Specific objects for test build
TEST_OBJS_TEST = $(TEST_SRCS:.c=.o)
TEST_TARGET = krep_test

TARGET = krep

.PHONY: all clean install uninstall test test-directory ci bench-rg

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

# Rule for main objects
%.o: %.c krep.h aho_corasick.h
	$(CC) $(CFLAGS) -c $< -o $@

# Rule for LMDB and index objects (no krep.h dependency)
lmdb/mdb.o: lmdb/mdb.c lmdb/lmdb.h lmdb/midl.h
	$(CC) $(CFLAGS) -c $< -o $@

lmdb/midl.o: lmdb/midl.c lmdb/midl.h
	$(CC) $(CFLAGS) -c $< -o $@

krep_index.o: krep_index.c krep_index.h lmdb/lmdb.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- Test Build ---
# Rule for test-specific main objects (compiled with -DTESTING)
krep_test.o: krep.c krep.h aho_corasick.h
	$(CC) $(CFLAGS) -DTESTING -c krep.c -o krep_test.o

aho_corasick_test.o: aho_corasick.c krep.h aho_corasick.h
	$(CC) $(CFLAGS) -DTESTING -c aho_corasick.c -o aho_corasick_test.o

# Rule for test file objects (compiled with -DTESTING)
test/%.o: test/%.c test/test_krep.h test/test_compat.h krep.h
	$(CC) $(CFLAGS) -DTESTING -c $< -o $@

# Link test executable
$(TEST_TARGET): $(TEST_OBJS_MAIN) $(TEST_OBJS_TEST)
	$(CC) $(CFLAGS) -DTESTING -o $(TEST_TARGET) $(TEST_OBJS_MAIN) $(TEST_OBJS_TEST) $(LDFLAGS) -lm # Add -lm if needed

test: $(TEST_TARGET)
	./$(TEST_TARGET)

test-directory: test_directory
	./test_directory

ci: all test test-directory

bench-rg: $(TARGET)
	bash test/benchmark_krep_vs_rg.sh

test_directory: test/test_directory.c krep.c aho_corasick.c
	$(CC) $(CFLAGS) -DTESTING -o $@ $^ $(LDFLAGS)

all-tests: test_basic test_krep test_regex test_multiple_patterns test_directory

# --- Installation ---
install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# --- Cleanup ---
clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJS) $(TEST_OBJS_MAIN) $(TEST_OBJS_TEST) *.o test/*.o lmdb/*.o
