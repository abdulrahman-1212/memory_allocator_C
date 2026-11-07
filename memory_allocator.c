/*
The challenges in memory allocation:
    1. Tracking free blocks
    2. Finding the suitable block
    3. Handling fragmentation
    4. Alignment requirements
    5. Overhead minimization
*/

#include <stddef.h>   // for size_t, NULL, ptrdiff definitions
#include <stdint.h>   // for fixed-width integers
#include <unistd.h>   // for POSIX (portable operating system interface)
#include <stdbool.h>
#include <string.h>   // for memset, memcmp
#include <stdlib.h>   // for rand, srand (include BEFORE the macros below)
#include <stdio.h>
#include <pthread.h>

#ifdef TESTING
#define malloc my_malloc
#define free   my_free
#endif

#define NTHREADS 4
#define TSLOTS   16

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

#define ALIGNMENT 16
#define ALIGN_UP(x) (((x) + (ALIGNMENT - 1)) & ~(size_t)(ALIGNMENT - 1))

typedef struct block {
    size_t size;
    struct block* next;
    struct block* prev;
    bool is_free;
} block_t;

#define HEADER_SIZE sizeof(block_t)

static block_t* heap_head = NULL;
static block_t* heap_tail = NULL;

static block_t* find_free_block(size_t size) {
    // first-fit strategy
    block_t* current = heap_head;

    while(current) {
        if (current->is_free && current->size >= size) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

static void coalesce_blocks(block_t* block);   

static block_t* request_memory(size_t size) {
    static size_t page_size = 0;

    if (!page_size) {
        page_size = sysconf(_SC_PAGESIZE);

        uintptr_t current_break = (uintptr_t)sbrk(0);
        size_t pad = ALIGN_UP(current_break) - current_break;
        if (pad && sbrk(pad) == (void*)-1) {
            page_size = 0;
            return NULL;
        }
    }

    if (size > SIZE_MAX - sizeof(block_t) - page_size) return NULL; //overflow
    size_t total_size = size + sizeof(block_t);
    size_t algined_size = ((total_size + page_size - 1) / page_size) * page_size;

    void *memory = sbrk(algined_size);
    if (memory == (void*)-1) {
        return NULL;
    }

    block_t *new_block = (block_t*)memory;
    new_block->size = algined_size - sizeof(block_t);
    new_block->is_free = true;
    new_block->next = NULL;
    new_block->prev = heap_tail;
    
    if (!heap_head) {
        heap_head = new_block;
    } else {
        heap_tail->next = new_block;
    }

    heap_tail = new_block;

    coalesce_blocks(new_block);

    return heap_tail;

}

static void split_block(block_t* block, size_t size) {
    if (block->size - size > sizeof(block_t) + ALIGNMENT) {
        block_t* new_block = (block_t*)((char*)block + sizeof(block_t) + size);
        new_block->is_free = true;
        new_block->size = block->size - sizeof(block_t) - size;

        new_block->next = block->next;
        new_block->prev = block;
        
        if (new_block->next)
            new_block->next->prev = new_block;
        else
            heap_tail = new_block;

        block->size = size;
        block->next = new_block;

    }
}

void* malloc(size_t size) {
    if (size == 0) return NULL;
    if (size > SIZE_MAX - sizeof(block_t) - 2 * ALIGNMENT) return NULL;   // overflow


    size = ALIGN_UP(size);

    pthread_mutex_lock(&heap_lock);

    block_t *block = find_free_block(size);

   if(!block){
        block = request_memory(size);
        if(!block) {
            pthread_mutex_unlock(&heap_lock);
            return NULL;
        }
    }

    split_block(block, size);
    block->is_free = false;

    pthread_mutex_unlock(&heap_lock);

    return (void*)((char*)block + sizeof(block_t));
}


static void coalesce_blocks(block_t* block) {
    // Coalesce with next block if it's free
    if (block->next && block->next->is_free &&
        (char*)block + sizeof(block_t) + block->size == (char*)block->next) {
        block->size += sizeof(block_t) + block->next->size;
        block->next = block->next->next;

        if (block->next) {
            block->next->prev = block;
        } else {
            heap_tail = block;
        }
    }

    // coalesce prev blocks
    if (block->prev && block->prev->is_free &&
        (char*)block->prev + sizeof(block_t) + block->prev->size == (char*)block) {
        block_t *prev = block->prev;

        prev->size += sizeof(block_t) + block->size;
        prev->next = block->next;

        if (prev->next) {
            prev->next->prev = prev;
        } else {
            heap_tail = prev;
        }
    }

}

void free(void* ptr) {
    if (!ptr) return;

    block_t* block = (block_t*)((char*)ptr - sizeof(block_t)); 

    pthread_mutex_lock(&heap_lock);
    if(!block->is_free) {
        block->is_free = true;
        coalesce_blocks(block);
    }
    pthread_mutex_unlock(&heap_lock);
}

void print_allocator_state(void) {
    pthread_mutex_lock(&heap_lock);
    printf("=== Allocator State ===\n");

    block_t *current = heap_head;

    while(current) {
        printf(
            "Block: %p | Size: %zu | Free: %s | Prev: %p | Next: %p\n",
            (void *)current,
            current->size,
            current->is_free ? "yes" : "no",
            (void *)current->prev,
            (void *)current->next
        );

        current = current->next;
    }

    pthread_mutex_unlock(&heap_lock);

}


#ifdef TESTING

static int failures = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) printf("  [ok]   %s\n", msg);                       \
        else { printf("  [FAIL] %s (line %d)\n", msg, __LINE__);      \
               failures++; }                                          \
    } while (0)

static size_t count_blocks(size_t *free_count) {
    size_t n = 0, f = 0;
    for (block_t *b = heap_head; b; b = b->next) {
        n++;
        if (b->is_free) f++;
    }
    if (free_count) *free_count = f;
    return n;
}

// Walks the whole list and verifies the structural invariants.
static bool heap_is_consistent(void) {
    block_t *prev = NULL;
    for (block_t *b = heap_head; b; prev = b, b = b->next) {
        if (b->prev != prev) return false;                       // prev link wrong
        if (b->size % ALIGNMENT != 0) return false;              // misaligned size
        if ((uintptr_t)b % ALIGNMENT != 0) return false;         // misaligned header
        if (prev && prev->is_free && b->is_free &&
            (char*)prev + sizeof(block_t) + prev->size == (char*)b)
            return false;                                        // missed coalesce
        if (!b->next && heap_tail != b) return false;            // stale tail
    }
    return prev == heap_tail;
}

static void fill(void *p, size_t n, unsigned char v) { memset(p, v, n); }

static bool verify(const void *p, size_t n, unsigned char v) {
    const unsigned char *c = p;
    for (size_t i = 0; i < n; i++)
        if (c[i] != v) return false;
    return true;
}

static void test_basic(void) {
    printf("\n[test] basic allocation\n");
    void *a = malloc(100), *b = malloc(200), *c = malloc(50);

    CHECK(a && b && c, "allocations succeed");
    CHECK(a != b && b != c && a != c, "pointers are distinct");
    CHECK((uintptr_t)a % ALIGNMENT == 0 &&
          (uintptr_t)b % ALIGNMENT == 0 &&
          (uintptr_t)c % ALIGNMENT == 0, "payloads are 16-byte aligned");

    fill(a, 100, 0xAA); fill(b, 200, 0xBB); fill(c, 50, 0xCC);
    CHECK(verify(a, 100, 0xAA) && verify(b, 200, 0xBB) && verify(c, 50, 0xCC),
          "blocks don't overlap (patterns intact)");
    CHECK(heap_is_consistent(), "heap is consistent");

    // a, b, c plus the free remainder of the first chunk
    size_t free_count;
    size_t total = count_blocks(&free_count);
    CHECK(total == 4 && free_count == 1, "split left 3 used blocks + 1 free tail");

    print_allocator_state();

    printf("\n[test] coalescing\n");
    free(b);
    CHECK(heap_is_consistent(), "consistent after freeing middle block");
    free(a);
    CHECK(count_blocks(&free_count) == 3, "a + b merged (backward merge)");
    CHECK(heap_is_consistent(), "consistent after merge with previous");
    free(c);
    total = count_blocks(&free_count);
    CHECK(total == 1 && free_count == 1, "everything merged into one free block");
    CHECK(heap_head == heap_tail, "head == tail");
    CHECK(heap_is_consistent(), "consistent after full coalesce");

    print_allocator_state();
}

static void test_reuse_and_split(void) {
    printf("\n[test] reuse and split\n");
    void *first = malloc(100);
    CHECK((char*)first - sizeof(block_t) == (char*)heap_head,
          "first-fit reuses the start of the heap");
    void *second = malloc(100);
    CHECK((char*)second > (char*)first, "second block is carved after the first");

    free(first);
    void *again = malloc(64);   // smaller: should reuse and split the hole
    CHECK(again == first, "freed block is reused for a smaller request");
    CHECK(heap_is_consistent(), "consistent after reuse + split");

    free(again);
    free(second);
    CHECK(count_blocks(NULL) == 1, "heap collapses back to a single block");
}

static void test_edge_cases(void) {
    printf("\n[test] edge cases\n");
    CHECK(malloc(0) == NULL, "malloc(0) returns NULL");
    CHECK(malloc(SIZE_MAX) == NULL, "malloc(SIZE_MAX) returns NULL");
    CHECK(malloc(SIZE_MAX - 8) == NULL, "near-SIZE_MAX request returns NULL");
    free(NULL);
    CHECK(1, "free(NULL) is a no-op");

    void *p = malloc(32);
    free(p);
    free(p);                    // double free: must be ignored, not corrupt
    CHECK(heap_is_consistent(), "double free doesn't corrupt the heap");

    void *one = malloc(1);
    CHECK(one && (uintptr_t)one % ALIGNMENT == 0, "1-byte request is aligned");
    free(one);
}

static void test_growth(void) {
    printf("\n[test] heap growth\n");
    size_t big = 1 << 20;       // 1 MiB: far larger than the first chunk
    unsigned char *p = malloc(big);
    CHECK(p != NULL, "1 MiB allocation succeeds");
    fill(p, big, 0x5A);
    CHECK(verify(p, big, 0x5A), "all 1 MiB is writable");
    CHECK(heap_is_consistent(), "consistent after growth");

    void *q = malloc(100);      // must not land inside p
    CHECK(q && ((char*)q + 100 <= (char*)p || (char*)q >= (char*)p + big),
          "later allocation doesn't overlap the big block");
    free(q);
    free(p);
    CHECK(heap_is_consistent(), "consistent after freeing big block");
}

#define SLOTS 512
static void test_stress(void) {
    printf("\n[test] randomized stress\n");
    static void *ptr[SLOTS];
    static size_t len[SLOTS];
    static unsigned char tag[SLOTS];
    bool ok = true;
    srand(12345);

    for (int iter = 0; iter < 20000 && ok; iter++) {
        int i = rand() % SLOTS;
        if (ptr[i]) {
            if (!verify(ptr[i], len[i], tag[i])) ok = false;   // corruption?
            free(ptr[i]);
            ptr[i] = NULL;
        } else {
            len[i] = 1 + (size_t)(rand() % 2000);
            tag[i] = (unsigned char)(rand() & 0xFF);
            ptr[i] = malloc(len[i]);
            if (!ptr[i] || (uintptr_t)ptr[i] % ALIGNMENT) { ok = false; break; }
            fill(ptr[i], len[i], tag[i]);
        }
        if (iter % 500 == 0 && !heap_is_consistent()) ok = false;
    }
    CHECK(ok, "20000 random malloc/free ops: no corruption");

    for (int i = 0; i < SLOTS; i++) {
        if (ptr[i]) {
            if (!verify(ptr[i], len[i], tag[i])) ok = false;
            free(ptr[i]);
        }
    }
    size_t free_count;
    size_t total = count_blocks(&free_count);
    CHECK(ok, "surviving blocks still intact when freed");
    CHECK(total == free_count, "no used blocks remain");
    CHECK(heap_is_consistent(), "heap consistent at the end");
    printf("  final heap: %zu block(s)\n", total);
}


static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;   // rand_r: per-thread state
    void *ptr[TSLOTS] = {0};
    size_t len[TSLOTS];
    unsigned char tag[TSLOTS];
    bool ok = true;

    for (int iter = 0; iter < 50000 && ok; iter++) {
        int i = rand_r(&seed) % TSLOTS;
        if (ptr[i]) {
            if (!verify(ptr[i], len[i], tag[i])) ok = false;   // someone overwrote us
            free(ptr[i]);
            ptr[i] = NULL;
        } else {
            len[i] = 1 + rand_r(&seed) % 2000;
            tag[i] = (unsigned char)(rand_r(&seed) & 0xFF);
            ptr[i] = malloc(len[i]);
            if (!ptr[i] || (uintptr_t)ptr[i] % ALIGNMENT) { ok = false; break; }
            fill(ptr[i], len[i], tag[i]);
        }
    }
    for (int i = 0; i < TSLOTS; i++) {
        if (ptr[i]) {
            if (!verify(ptr[i], len[i], tag[i])) ok = false;
            free(ptr[i]);
        }
    }
    return ok ? NULL : (void *)1;
}

static void test_threads(void) {
    printf("\n[test] multithreaded stress\n");
    pthread_t t[NTHREADS];
    bool created[NTHREADS] = {0};
    bool ok = true;

    for (int i = 0; i < NTHREADS; i++)
        created[i] = pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i + 1)) == 0;
    for (int i = 0; i < NTHREADS; i++) {
        if (!created[i]) { ok = false; continue; }
        void *ret;
        pthread_join(t[i], &ret);
        if (ret) ok = false;
    }

    size_t free_count;
    size_t total = count_blocks(&free_count);
    CHECK(ok, "8 threads x 50000 ops: no corruption");
    CHECK(total == free_count, "no used blocks remain");
    CHECK(heap_is_consistent(), "heap consistent after all threads joined");
}


int main(void) {
    // Give stdout its own buffer so printf never calls glibc's malloc,
    // which would move the program break underneath our allocator.
    pthread_mutex_lock(&heap_lock);
    pthread_mutex_unlock(&heap_lock);

    static char outbuf[BUFSIZ];
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);

    test_basic();
    test_reuse_and_split();
    test_edge_cases();
    test_growth();
    test_stress();
    test_threads();
    printf("\n%s (%d failure%s)\n",
           failures ? "SOME TESTS FAILED" : "ALL TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    fflush(stdout);
    return failures != 0;
}

#endif /* TESTING */