#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

// Fix to map_anonymous error
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);           \
    } while (0)

/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
size_t btok(size_t bytes)
{
    for (int i = SMALLEST_K; i < MAX_K; i++)
    {
        int num_bytes = UINT64_C(1) << i;
        if (bytes <= num_bytes)
        {
            return (size_t)i;
        }
    }
    return MAX_K; // Highest value possible for this assignment
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    size_t buddy_size = UINT64_C(1) << buddy->kval;
    // Find the an offset to transition into a local frame
    uintptr_t offset = (uintptr_t)buddy - (uintptr_t)pool->base;
    // Get the local offset of the buddy
    uintptr_t offset_buddy = offset ^ buddy_size;
    // Get the buddy's address back into a global frame
    return (struct avail *)((uintptr_t)pool->base + offset_buddy);
}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (pool == NULL || size == 0)
    {
        return NULL;
    }

    // get the kval for the requested size with enough room for the tag and kval fields
    size_t requested_k = btok(size + sizeof(struct avail));

    // R1 Find a block
    int index = requested_k;
    while (index <= pool->kval_m && pool->avail[index].next == &pool->avail[index])
    {
        index++;
    }

    // There was not enough memory to satisfy the request thus we need to set error and return NULL
    if (index > pool->kval_m)
    {
        errno = ENOMEM;
        return NULL;
    }

    // R2 Remove from list;
    struct avail *returned_block = pool->avail[index].next;
    struct avail *next_block = returned_block->next;

    // Disconnect the block we intend to return
    pool->avail[index].next = next_block;
    next_block->prev = &pool->avail[index];

    // Update data of allocated block
    returned_block->tag = BLOCK_RESERVED;
    returned_block->kval = index;

    // R3 Split required?
    while (index > requested_k)
    { // Continue to split block until it is the correct size
        index--;

        // R4: Split block
        uintptr_t addr = (uintptr_t)returned_block;
        uintptr_t buddy_addr = addr + (1UL << index);

        struct avail *buddy = (struct avail *)buddy_addr;
        buddy->tag = BLOCK_AVAIL;
        buddy->kval = index;

        // Insert buddy into the avail[index] list
        buddy->next = pool->avail[index].next;
        buddy->prev = &pool->avail[index];
        pool->avail[index].next->prev = buddy;
        pool->avail[index].next = buddy;

        returned_block->kval = index;
    }
    return (void *)(returned_block + UINT64_C(1));
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (ptr == NULL)
    {
        return NULL;
    }

    struct avail *free_ptr = (struct avail *)((char *)ptr - sizeof(struct avail)); // move full sizeof(struct avail) back instead of 2 short int
    struct avail *buddy_ptr = buddy_calc(pool, free_ptr);

    // Merge block back with its buddy as long as much as possible
    while (free_ptr->kval < pool->kval_m && buddy_ptr->tag == BLOCK_AVAIL && buddy_ptr->kval == free_ptr->kval)
    {
        // Remove the buddy from the available blocks
        buddy_ptr->prev->next = buddy_ptr->next;
        buddy_ptr->next->prev = buddy_ptr->prev;

        // Merge the two blocks
        if (buddy_ptr < free_ptr)
        {
            free_ptr = buddy_ptr;
        }

        // Update new block's size and attempt to find its buddy
        free_ptr->kval++;
        buddy_ptr = buddy_calc(pool, free_ptr);
    }

    // Mark final block as available and return it to the avail list
    free_ptr->tag = BLOCK_AVAIL;
    size_t kval = free_ptr->kval;
    struct avail *head = &pool->avail[kval];

    // Attach it to a block in the avail list
    free_ptr->next = head->next;
    free_ptr->prev = head;
    head->next->prev = free_ptr;
    head->next = free_ptr;
}

/**
 * @brief This is a simple version of realloc.
 *
 * @param poolThe memory pool
 * @param ptr  The user memory
 * @param size the new size requested
 * @return void* pointer to the new user memory
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
{
    // Required for Grad Students
    // Optional for Undergrad Students
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    // make sure pool struct is cleared out
    memset(pool, 0, sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    // Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                        /*addr to map to*/
        pool->numbytes,              /*length*/
        PROT_READ | PROT_WRITE,      /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS, /*flags*/
        -1,                          /*fd -1 when using MAP_ANONYMOUS*/
        0                            /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    // Set all blocks to empty. We are using circular lists so the first elements just point
    // to an available block. Thus the tag, and kval feild are unused burning a small bit of
    // memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    // Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    // Zero out the array so it can be reused it needed
    memset(pool, 0, sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
static void printb(unsigned long int b)
{
    size_t bits = sizeof(b) * 8;
    unsigned long int curr = UINT64_C(1) << (bits - 1);
    for (size_t i = 0; i < bits; i++)
    {
        if (b & curr)
        {
            printf("1");
        }
        else
        {
            printf("0");
        }
        curr >>= 1L;
    }
}
