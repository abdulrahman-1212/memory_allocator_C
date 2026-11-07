# Memory Allocator in C

A from-scratch, thread-safe `malloc`/`free` implementation for Linux, built on `sbrk`. It was written to learn how heap allocators work: block metadata, free-block search, splitting, coalescing, alignment, and locking.

## Features

- **Address-ordered doubly linked list** of blocks, so physical neighbors are always the list neighbors
- **First-fit** search for a free block
- **Block splitting** when a free block is larger than the request
- **Coalescing** with the next and previous block on `free`, with a physical-adjacency check so blocks separated by foreign `sbrk` usage are never merged
- **16-byte aligned** payloads (matches the x86-64 `malloc` guarantee)
- **Overflow-safe** size handling, `malloc(0)` returns `NULL`, `free(NULL)` is a no-op, and double frees are ignored
- **Thread-safe** through a single global `pthread_mutex_t`

## How it works

Each allocation is preceded by a header:

```c
typedef struct block {
    size_t size;          // payload size in bytes
    struct block* next;   // next block in address order
    struct block* prev;   // previous block in address order
    bool is_free;
} block_t;                // 32 bytes on 64-bit
```

| Operation | Behavior |
|-----------|----------|
| `malloc`  | Round the size up to 16 bytes, find a free block (first fit). If none fits, grow the heap with `sbrk` (page-rounded), then split off any excess and mark the block used. |
| `free`    | Mark the block free and merge it with adjacent free neighbors. |

On first use the allocator aligns the program break to 16 bytes and caches the page size.

## Build and test

The test harness is compiled in with `-DTESTING`. This renames the functions to `my_malloc`/`my_free` so they don't replace libc's own allocator.

```bash
gcc -DTESTING -g -Wall -Wextra -pthread memory_allocator.c -o test_alloc
./test_alloc
```

Optional race detection:

```bash
gcc -DTESTING -g -pthread -fsanitize=thread memory_allocator.c -o test_tsan
./test_tsan
```

On WSL2, TSan may fail to start because of ASLR settings. Try `setarch $(uname -m) -R ./test_tsan`, or use `valgrind --tool=helgrind ./test_alloc`.

### What the tests cover

- Basic allocation, distinct pointers, 16-byte alignment, and no overlap between blocks (pattern fill)
- Forward and backward coalescing, and reuse and splitting of freed blocks
- Edge cases: `malloc(0)`, `malloc(SIZE_MAX)`, `free(NULL)`, double free, 1-byte requests
- Heap growth with a 1 MiB allocation
- A randomized single-thread stress test and a multi-threaded stress test
- A `heap_is_consistent()` checker that validates `prev`/`next` links, alignment, `heap_tail`, and that no adjacent free blocks were left unmerged

## Limitations

- **Performance:** one global lock and an O(n) first-fit scan over all blocks (used and free)
- **Memory return:** memory is never returned to the OS, and large requests are not served with `mmap`
- **Growth:** the heap grows by page-rounded request sizes, so there is one `sbrk` call per growth
- **Interposition:** without `-DTESTING` the file defines `malloc`/`free` directly, but `calloc`, `realloc`, and `posix_memalign` are not implemented, so the file should not be used as a drop-in libc replacement
- **Other:** no `fork` handling (`pthread_atfork`), and no `printf` inside the allocator if it is used as the real `malloc` (it can recurse)

## Possible next steps

- Grow the heap in larger chunks (for example 1 MiB) to cut `sbrk` calls
- Use `mmap`/`munmap` for large allocations
- Replace the single list with segregated free lists and boundary tags
- Add per-thread caches to reduce lock contention
- Implement `calloc`, `realloc`, and `posix_memalign`
