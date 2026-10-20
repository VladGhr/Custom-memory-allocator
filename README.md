# Custom Memory Allocator

A minimal user-space implementation of the C dynamic memory API (`malloc`, `free`, `calloc`, `realloc`) built directly on top of the Linux system calls `brk`/`sbrk` and `mmap`/`munmap`.

## Table of Contents

- [Overview](#overview)
- [Public API](#public-api)
- [Design](#design)
  - [Block metadata](#block-metadata)
  - [Two memory sources](#two-memory-sources)
  - [Allocation strategy](#allocation-strategy)
  - [Splitting and coalescing](#splitting-and-coalescing)
  - [Realloc behavior](#realloc-behavior)
- [Constants](#constants)
- [Project Structure](#project-structure)
- [Building and Usage](#building-and-usage)
- [Known Limitations](#known-limitations)
- [License](#license)

## Overview

The allocator manages memory through a linked list of blocks, each preceded by a small metadata header. Small requests are served from a heap grown with `sbrk`, while large requests get their own anonymous `mmap` region that is returned to the OS on `free`.

Key features:

- 8-byte aligned payloads
- Heap preallocation (128 KB) on first use to reduce `sbrk` calls
- First-fit search over free blocks
- Block splitting to limit internal fragmentation
- Coalescing of adjacent free blocks to limit external fragmentation
- In-place `realloc` whenever possible (shrink, expand into a free neighbor, or extend the last block)

## Public API

```c
void *os_malloc(size_t size);
void  os_free(void *ptr);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);
```

| Function | Behavior |
|----------|----------|
| `os_malloc(size)` | Returns an aligned block of at least `size` bytes, or `NULL` if `size == 0`. |
| `os_free(ptr)` | `NULL` is a no-op. Heap blocks are marked free for reuse; mapped blocks are unmapped with `munmap`. |
| `os_calloc(nmemb, size)` | Like `os_malloc(nmemb * size)` but the payload is zero-initialized. Returns `NULL` for a zero-sized request. |
| `os_realloc(ptr, size)` | `ptr == NULL` behaves like `os_malloc`; `size == 0` behaves like `os_free` and returns `NULL`; otherwise resizes the block, moving it only if necessary. Returns `NULL` for an already-freed block. |

## Design

### Block metadata

Every allocation is preceded by a `struct block_meta` header (defined in `block_meta.h`) holding:

- `size`: payload size in bytes
- `status`: one of `STATUS_FREE`, `STATUS_ALLOC`, `STATUS_MAPPED`
- `next` / `prev`: links in a doubly linked list

The user pointer is the address immediately after the header:

```
 +-------------------+------------------------------+
 |   struct          |          payload             |
 |   block_meta      |   (aligned to 8 bytes)       |
 +-------------------+------------------------------+
 ^                   ^
 block               pointer returned to the user
```

`META_SIZE` is the header size rounded up to a multiple of 8 so payloads stay aligned.

### Two memory sources

| Source | Used when | Status | Released by |
|--------|-----------|--------|-------------|
| `sbrk` (heap) | `size + META_SIZE <= MMAP_THRESHOLD` | `STATUS_ALLOC` / `STATUS_FREE` | Stays in the heap, reused later |
| `mmap` (anonymous) | `size + META_SIZE > MMAP_THRESHOLD` | `STATUS_MAPPED` | `munmap` on `os_free` |

Heap blocks and mapped blocks are tracked in **separate lists**, each with its own base and tail pointer (`global_base_alloc` / `last_memory_block_alloc` and `global_base_map` / `last_memory_block_map`).

On the very first small allocation, the allocator calls `sbrk(MMAP_THRESHOLD)` once to preallocate a 128 KB heap and carves the first block out of it.

> `os_calloc` uses a page size (4 KB) as its mmap threshold instead of `MMAP_THRESHOLD`, so large zeroed allocations go to `mmap` earlier than `os_malloc` ones.

### Allocation strategy

For a small request, the allocator:

1. Coalesces adjacent free blocks.
2. Scans the heap list for the **first free block** large enough (first fit).
3. If the free block is the **last** block of the heap and too small, it is extended with `sbrk` instead of allocating a new block.
4. If a suitable block is much larger than needed, it is split (see below).
5. If nothing fits, a new block is obtained with `sbrk(size + META_SIZE)` and appended to the list.

### Splitting and coalescing

- **Splitting:** when a free block is larger than `size + META_SIZE + 8`, the remainder becomes a new free block linked right after it.
- **Coalescing:** before each heap search, consecutive free blocks are merged into one, combining their sizes plus the removed header.

```
Before split:  [hdr | ........ 256 ........]
After split:   [hdr | .. 64 ..][hdr | .. 184 ..]
                  used            free
```

### Realloc behavior

For a heap block (`STATUS_ALLOC`), the allocator tries, in order:

1. **Shrink in place**: split the block and return the same pointer.
2. **Fits already**: return the same pointer.
3. **Last block of the heap**: grow it with `sbrk`.
4. **Next block is free and large enough**: absorb it, then split off any excess.
5. **Fallback**: `os_malloc` a new block, `memcpy` the smaller of the two sizes, and `os_free` the old one.

For a mapped block (`STATUS_MAPPED`), a new block is allocated and the old mapping is released.

## Constants

| Macro | Value | Meaning |
|-------|-------|---------|
| `MMAP_THRESHOLD` | `128 * 1024` | Boundary between `sbrk` and `mmap` allocations, also the initial heap size |
| `ALIGN(size)` | round up to multiple of 8 | Alignment of sizes and headers |
| `META_SIZE` | `ALIGN(sizeof(struct block_meta))` | Aligned metadata header size |
| `PAGESIZE` | `ALIGN(4096)` | `mmap` threshold used by `os_calloc` |

## Project Structure

```
.
├── osmem.c          # Allocator implementation
├── osmem.h          # Public API declarations
├── block_meta.h     # struct block_meta and status constants
└── README.md
```

## Building and Usage

The allocator targets Linux (it relies on `sbrk` and `mmap`). Compile the implementation together with your program:

```bash
gcc -Wall -Wextra -c osmem.c -o osmem.o
gcc main.c osmem.o -o app
```

Example:

```c
#include "osmem.h"

int main(void)
{
    int *arr = os_malloc(10 * sizeof(int));
    arr = os_realloc(arr, 20 * sizeof(int));
    os_free(arr);

    char *zeros = os_calloc(100, sizeof(char));
    os_free(zeros);
    return 0;
}
```

