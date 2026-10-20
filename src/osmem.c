#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "osmem.h"
#include "block_meta.h"



void *os_malloc(size_t size);
void os_free(void *ptr);
void *os_calloc(size_t nmemb, size_t size);
void *os_realloc(void *ptr, size_t size);


#define MMAP_THRESHOLD (128 * 1024) // 128kb
#define MAX_MEMORY (999 * MMAP_THRESHOLD)
#define ALIGN(size) (((size) + (8 - 1)) & ~(8 - 1))
#define META_SIZE ALIGN(sizeof(struct block_meta))
#define MAP_ANON MAP_ANONYMOUS
#define PAGESIZE ALIGN(4096)

void *global_base_alloc;
void *global_base_map;
struct block_meta *last_memory_block_map;
struct block_meta *last_memory_block_alloc;

int is_realloc;

struct block_meta *get_block_ptr(void *ptr)
{
	return (struct block_meta *)ptr - 1;
	
}

void coalesce_memory(void)
{
	struct block_meta *current = get_block_ptr(global_base_alloc);

	while (current && current->next) {
		if (current->status == STATUS_FREE && current->next->status == STATUS_FREE) {
			current->size += current->next->size + META_SIZE;
			current->next = current->next->next;
			if (current == current->next)
				return;
		} else {
			current = current->next;
		}
	}
}

struct block_meta *split_memory_block(struct block_meta *addr, size_t size, int is_last)
{
	struct block_meta *next;
	struct block_meta *new_memory_block;
	size_t next_size;

	new_memory_block = (struct block_meta *)(((void *)addr) + size + META_SIZE);

	if (addr->size <= META_SIZE + size)
		return addr;

	if (addr->next != NULL)
		next_size = addr->next->size;

	if (is_last == 0) {
		new_memory_block->size = addr->size - META_SIZE - size;
		new_memory_block->next = addr->next;
		addr->next = new_memory_block;
		new_memory_block->prev = addr;
		addr->size -= new_memory_block->size;

		new_memory_block->status = STATUS_FREE;
	} else {
		new_memory_block->size = addr->size - META_SIZE - size;
		new_memory_block->next = addr->next;
		addr->next = new_memory_block;
		new_memory_block->prev = addr;
		addr->size -= new_memory_block->size;

		last_memory_block_alloc = new_memory_block;
		new_memory_block->status = STATUS_FREE;
	}

	next = new_memory_block->next;

	if (new_memory_block->next && new_memory_block->next->size != next_size && is_realloc != 0)
		next->size = next_size;

	return addr;
}

struct block_meta *search_free_block_malloc(size_t size)
{
	struct block_meta *current;

	current = get_block_ptr(global_base_alloc);

	while (current) {
		if (current == current->next)
			return NULL;

		if (current->status == STATUS_FREE) {
			if (current == last_memory_block_alloc) {
				if (current->size < size) {
					sbrk(size - current->size);
					current->size = size;
				} else {
					if (current->size >= 8 + META_SIZE + size)
						return split_memory_block(current, size, 1);

					else
						return current;
				}

				return current;
			}
				if (current->size >= size) {
					if (current->size >= 8 + size + META_SIZE)
						return split_memory_block(current, size, 0);

					return current;
				}
					current = current->next;
		} else {
			current = current->next;
		}
	}

	return NULL;
}

struct block_meta *search_free_block_calloc(size_t size)
{
	struct block_meta *current;

	current = get_block_ptr(global_base_alloc);

	while (current) {
		if (current->status == STATUS_FREE) {
			if (current->next == NULL) {
				if (current->size < size) {
					sbrk(size - current->size);
					current->size = size;
				} else {
					if (current->size >= 8 + META_SIZE + size)
						return split_memory_block(current, size, 1);

					else
						return current;
				}

				return current;
			}
				if (current->size >= size) {
					if (current->size >= 8 + size + META_SIZE)
						return split_memory_block(current, size, 0);


					return current;
				}
				current = current->next;

		} else {
			current = current->next;
		}
	}

	return NULL;
}

struct block_meta *request_memory_malloc(size_t size)
{
	struct block_meta *memory_block;

	if (size + META_SIZE > MMAP_THRESHOLD) {
		if (global_base_map == NULL) {
			memory_block = mmap(0, size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

			memory_block->size = size;
			memory_block->status = STATUS_MAPPED;
			global_base_map = memory_block + 1;
			last_memory_block_map = memory_block;
			memory_block->next = NULL;
			memory_block->prev = NULL;

			return memory_block;
		}
			memory_block = mmap(0, size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

			memory_block->size = size;
			memory_block->status = STATUS_MAPPED;
			memory_block->next = NULL;
			memory_block->prev = last_memory_block_map;
			last_memory_block_map = memory_block;

			return memory_block;
	} else {
		if (global_base_alloc == NULL) {
			memory_block = sbrk(MMAP_THRESHOLD);

			global_base_alloc = memory_block + 1;
			last_memory_block_alloc = memory_block;

			memory_block->next = NULL;
			memory_block->prev = NULL;
			memory_block->size = size;
			memory_block->status = STATUS_ALLOC;

			return memory_block;
		}
			coalesce_memory();
			memory_block = search_free_block_malloc(size);
			if (memory_block == NULL) {
				memory_block = sbrk(size + META_SIZE);

				last_memory_block_alloc->next = memory_block;
				memory_block->prev = last_memory_block_alloc;
				last_memory_block_alloc = memory_block;

				memory_block->next = NULL;
				memory_block->size = size;
				memory_block->status = STATUS_ALLOC;

				return memory_block;
			}

			memory_block->status = STATUS_ALLOC;

			return memory_block;
	}
}

struct block_meta *request_memory_calloc(size_t size)
{
	struct block_meta *memory_block;

	if (size + META_SIZE > PAGESIZE) {
		if (global_base_map == NULL) {
			memory_block = mmap(0, size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

			memory_block->size = size;
			memory_block->status = STATUS_MAPPED;
			global_base_map = memory_block + 1;
			last_memory_block_map = memory_block;
			memory_block->next = NULL;
			memory_block->prev = NULL;

			return memory_block;
		}
			memory_block = mmap(0, size + META_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

			memory_block->size = size;
			memory_block->status = STATUS_MAPPED;
			memory_block->next = NULL;
			memory_block->prev = last_memory_block_map;
			last_memory_block_map = memory_block;

			return memory_block;
	} else {
		if (global_base_alloc == NULL) {
			memory_block = sbrk(MMAP_THRESHOLD);

			global_base_alloc = memory_block + 1;
			last_memory_block_alloc = memory_block;

			memory_block->next = NULL;
			memory_block->prev = NULL;
			memory_block->size = size;
			memory_block->status = STATUS_ALLOC;

			return memory_block;
		}
			coalesce_memory();
			memory_block = search_free_block_calloc(size);

			if (memory_block == NULL) {
				memory_block = sbrk(size + META_SIZE);

				last_memory_block_alloc->next = memory_block;
				memory_block->prev = last_memory_block_alloc;
				last_memory_block_alloc = memory_block;

				memory_block->next = NULL;
				memory_block->size = size;
				memory_block->status = STATUS_ALLOC;

				return memory_block;
			}

			memory_block->status = STATUS_ALLOC;

			return memory_block;
	}
}

void *os_malloc(size_t size)
{

	struct block_meta *memory_block = NULL;

	if (size <= 0)
		return NULL;

	size = ALIGN(size);
	memory_block = request_memory_malloc(size);

	return memory_block + 1;
}

void os_free(void *ptr)
{
	if (!ptr)
		return;

	struct block_meta *memory_block = get_block_ptr(ptr);

	if (memory_block->status == STATUS_ALLOC)
		memory_block->status = STATUS_FREE;

	else if (memory_block->status == STATUS_MAPPED)
		munmap(memory_block, memory_block->size + META_SIZE);
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t memory = ALIGN(nmemb * size);
	struct block_meta *memory_block;

	if (memory <= 0)
		return NULL;

	memory_block = request_memory_calloc(memory);

	if (memory_block)
		memset(memory_block + 1, 0, memory);

	return memory_block + 1;
}

void *os_realloc(void *ptr, size_t size)
{

	struct block_meta *memory_block = get_block_ptr(ptr);

	void *new_ptr;

	size_t smaller_size;

	is_realloc = 1;

	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	size = ALIGN(size);

	if (size > memory_block->size)
		smaller_size = memory_block->size;

	else
		smaller_size = size;

	if (memory_block->status == STATUS_FREE)
		return NULL;

	if (memory_block->status == STATUS_ALLOC) {
		if (memory_block->size >= size + 8) {
			split_memory_block(memory_block, size, last_memory_block_alloc == memory_block);
			return ptr;
		}

		if ((void *)memory_block->next >= (void *)ptr + size)
			return ptr;

		if (memory_block->next == NULL) {
			sbrk(ALIGN(size - memory_block->size));
			memory_block->size = size;
			return ptr;
		}

		if (memory_block->next) {
			if (memory_block->next->status == STATUS_FREE &&
			(memory_block->size + META_SIZE + memory_block->next->size >= size)) {
				memory_block->size = memory_block->size + META_SIZE + memory_block->next->size;
				memory_block->next = memory_block->next->next;
				if (last_memory_block_alloc == memory_block)
					return split_memory_block(memory_block, size, 1) + 1;
				return split_memory_block(memory_block, size, 0) + 1;
			}
		}

		new_ptr = os_malloc(size);

		memcpy(new_ptr, ptr, smaller_size);
		os_free(ptr);
		return new_ptr;
	}

	if (memory_block->status == STATUS_MAPPED) {
		void *new_ptr = os_malloc(size);

		munmap(memory_block, memory_block->size + META_SIZE);
		return new_ptr;
	}
	return NULL;
}
