/* 
 * The program works as a simple dynamic memory allocator, using 
 * 1. explicit free list, 
 * 2. first-fit, 
 * 3. immediate coalescing, and
 * 4. add new free block to the end of the list.
 * It uses a circular doubly linked list to search for free blocks.
 * 
 * It supports functions as an allocator including: malloc, calloc, realloc,
 * and free. It also provides a heap checker to help the debugging process.
 * 
 * Author: Jinyi Li
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "mm.h"
#include "memlib.h"

// #define DEBUG

#ifdef DEBUG
/* When debugging is enabled, the underlying functions get called */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#define dbg_checkheap(...) mm_checkheap(__VA_ARGS__)
#define dbg_printheap(...) print_heap(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated */
#define dbg_printf(...)
#define dbg_assert(...)
#define dbg_requires(...)
#define dbg_ensures(...)
#define dbg_checkheap(...)
#define dbg_printheap(...)
#endif

/* do not change the following! */
#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

#define ALIGNMENT 16

typedef uint64_t word_t;

struct block;

/* previous free block and next free block pointers */
typedef struct prev_next_ptrs
{
    struct block *prev;
    struct block *next;
} prev_next_ptrs;

/* block content union */
typedef union block_content {
    char payload[0];
    prev_next_ptrs ptrs;
} block_content;

/* block struct */
typedef struct block
{
    word_t header;
    block_content content;
} block_t;

/********************** PROTOTYPES **************************/
static size_t get_payload_size(block_t *block);
static block_t *extend_heap(size_t size);
static block_t *coalesce(block_t *block);
static block_t *find_fit(size_t asize);
static void place(block_t *block, size_t asize);
static void remove_from_list(block_t *target);
static void add_to_list(block_t *new);

static block_t *get_next_free(block_t *curr);
static block_t *get_prev_free(block_t *curr);
static void set_next_free(block_t *curr, block_t *next);
static void set_prev_free(block_t *curr, block_t *prev);

static block_t *get_next_block(block_t *curr);
static block_t *get_prev_block(block_t *curr);

static void write_block(block_t *block, size_t size, bool is_allocated);
static void *header_to_payload(block_t *block);
static block_t *payload_to_header(void *bp);
static size_t extract_size(word_t word);
static bool extract_alloc(word_t word);
static size_t get_size(block_t *block);
static bool get_alloc(block_t *block);
static word_t *find_prev_footer(block_t *block);

static word_t pack(size_t size, bool is_allocated);
static size_t round_up(size_t size, size_t n);
static size_t max(size_t x, size_t y);
static bool in_heap(const void *p);
static bool aligned(const void *p);

/********************** GLOBAL **************************/

/* Global constants */
static const size_t wsize = sizeof(word_t);     // word and header size (bytes)
static const size_t dsize = 2 * wsize;          // double word size (bytes)
static const size_t min_block_size = 2 * dsize; // Minimum block size
static const size_t chunksize = (1 << 12);      // requires (chunksize%16==0)

static const word_t alloc_mask = 0x1;         // one bit for the alloc flag
static const word_t size_mask = ~(word_t)0xF; // all bits except last four

/* Global variables */
static block_t *free_root = NULL;
static word_t *prologue = NULL;
static int counter_global = 0;


/****************** ALLOCATOR METHODS ****************************************/
/*
 * Initiate heap with prologue, root block for the free block list, and their
 * headers.
 */
bool mm_init(void)
{
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(5 * wsize));
    if (start == (void *)-1)
        return false;
    
    start[0] = pack(0, true); // Prologue
    start[1] = pack(0, true); // Root header
    start[4] = pack(0, true); // Root footer
    // Heap starts with first "block header", currently the epilogue footer
    prologue = (word_t *)&(start[0]);
    free_root = (block_t *)&(start[1]);

    // Initiate the circular doubly linked list
    free_root->content.ptrs.prev = free_root;
    free_root->content.ptrs.next = free_root;

    // Extend the empty heap with a free block of chunksize bytes
    if (extend_heap(chunksize) == NULL)
        return false;
    return true;
}

/*
 * Allocate a block with a required size, and return a pointer to the payload
 * of the block. It will return NULL if size <= 0.
 */
void *malloc(size_t size)
{
    dbg_printf(" malloc: required size %zu\n", size);
    dbg_requires(mm_checkheap(__LINE__));
    size_t asize;               // Adjusted block size
    size_t extendsize;          // Amount to extend heap if no fit is found
    block_t *block;
    void *bp = NULL;

    if (free_root == NULL)      // Initialize heap if it isn't initialized
        mm_init();
    if (size <= 0)              // Ignore spurious request
        return bp;

    // Adjust block size to include overhead and to meet alignment requirements
    asize = round_up(size + dsize, dsize);
    // Search the free list for a fit
    dbg_printf(" malloc: find_fit start\n");
    block = find_fit(asize);
    dbg_printf(" malloc: find_fit end\n");

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL)
    {
        extendsize = max(asize, chunksize);
        dbg_printf(" malloc: extend_heap start\n");
        block = extend_heap(extendsize);
        dbg_printf(" malloc: extend_heap end\n");
        if (block == NULL)      // extend_heap returns an error
            return bp;  
    }

    place(block, asize);
    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

/*
 * Free the block with the given pointer, which is assumed to be pointing to 
 * the payload of an allocated block. It will coalesce with its neighbors if
 * possible.
 */
void free(void *ptr)
{
    dbg_requires(ptr != NULL);
    block_t *block = payload_to_header(ptr);
    size_t size = get_size(block);

    write_block(block, size, false); // renew alloc bit
    dbg_requires(mm_checkheap(__LINE__));
    coalesce(block);
}

/*
 * Reallocate the requested block whose payload is pointed by ptr, with a 
 * reasonable size: min{size, block_size}. It will copy the content to the 
 * new block within the size range. 
 */
void *realloc(void *ptr, size_t size)
{
    block_t *block = payload_to_header(ptr);
    size_t copysize;
    void *newptr;

    // If size == 0, then free block and return NULL
    if (size == 0)
    {
        free(ptr);
        return NULL;
    }

    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL)
    {
        return malloc(size);
    }

    // Otherwise, proceed with reallocation
    newptr = malloc(size);
    // If malloc fails, the original block is left untouched
    if (newptr == NULL)
    {
        return NULL;
    }

    // Copy the old data
    copysize = get_payload_size(block); // gets size of old payload
    if (size < copysize)
    {
        copysize = size;
    }
    memcpy(newptr, ptr, copysize);

    // Free the old block
    free(ptr);

    return newptr;
}

/*
 * Allocates a block of memory for an array of num elements, each of them size 
 * bytes long, and initializes all its bits to zero.
 */
void *calloc(size_t elements, size_t size)
{
    void *bp;
    size_t asize = elements * size;

    if (asize / elements != size)
    {
        // Multiplication overflowed
        return NULL;
    }

    bp = malloc(asize);
    if (bp == NULL)
    {
        return NULL;
    }
    // Initialize all bits to 0
    memset(bp, 0, asize);

    return bp;
}


/****************** STATIC METHODS *******************************************/
/*
 * Get the size of payload of the given block 
 */
static size_t get_payload_size(block_t *block)
{
    size_t asize = get_size(block);
    return asize - dsize;
}

/*
 * Extend heap with the requested size. 
 */
static block_t *extend_heap(size_t size)
{
    void *bp;

    // Allocate an even number of words to maintain alignment
    size = round_up(size, dsize);
    if ((bp = mem_sbrk(size)) == (void *)-1)
        return NULL;
    dbg_printf("extend heap: entering\n");        
    block_t *new_block = (block_t *) bp;
    
    write_block(new_block, size, false);
    dbg_printf("************* ex_heap() write_block: finish\n");
    
    add_to_list(new_block);
    dbg_printf("add_to_list() end\n");
    dbg_requires(mm_checkheap(__LINE__));
    return new_block;
}

/*
 * Coalesce neighborhood blocks, physically.
 */
static block_t *coalesce(block_t *block)
{
    bool prev_alloc, next_alloc;

    dbg_printf("coal() start\n");
    block_t *next_block = get_next_block(block);
    block_t *prev_block = get_prev_block(block);
    dbg_printf("Coalesce: next pointer: %p\n", next_block);
    dbg_printf("Coalesce: pre pointer: %p\n", prev_block);

    // Check address validation - whether in heap
    if(in_heap(prev_block) && prev_block != NULL)
    {
        prev_alloc = extract_alloc(*(find_prev_footer(block)));
    }
    else
    {
        prev_alloc = true;
    }

    if(in_heap(next_block) && next_block != NULL)
    {
        next_alloc = get_alloc(next_block);
    }
    else
    {
        next_alloc = true;
    }
    
    // Coalesce cases - depending on the prev and next 
    // block's allocation status
    size_t size = get_size(block);    
    dbg_requires(mm_checkheap(__LINE__));

    if (prev_alloc && next_alloc) // Case 1
    {        
        add_to_list(block);
        return block;
    }
    else if (prev_alloc && !next_alloc) // Case 2
    {        
        remove_from_list(next_block);
        size += get_size(next_block);
        write_block(block, size, false);
        add_to_list(block);
    }
    else if (!prev_alloc && next_alloc) // Case 3
    {        
        remove_from_list(prev_block);
        size += get_size(prev_block);
        write_block(prev_block, size, false);
        block = prev_block;
        add_to_list(block);
    }
    else // Case 4
    {        
        remove_from_list(prev_block);
        remove_from_list(next_block);
        size += get_size(next_block) + get_size(prev_block);
        write_block(prev_block, size, false);        

        block = prev_block;
        add_to_list(block);
    }

    dbg_requires(mm_checkheap(__LINE__));
    return block;
}

/*
 * Find a fit block in the free block list with a given size asize.
 * Return the pointer to this fit block or NULL if not found.
 */
static block_t *find_fit(size_t asize)
{
    dbg_requires(mm_checkheap(__LINE__));
    block_t *block;

    for (block = get_next_free(free_root); get_size(block) > 0;
         block = get_next_free(block))
    {
        dbg_printf("block: %p\n", block);
        size_t size = get_size(block);
        if (asize <= size)
            return block;
    }
    return NULL;
}

/*
 * Place block as allocated with the requested block and asize.
 */
static void place(block_t *block, size_t asize)
{
    size_t block_size = get_size(block);
    if ((block_size - asize) >= min_block_size)
    {
        remove_from_list(block);
        write_block(block, asize, true); // renew the block size and alloc bit
        
        // the remining part is a new free block
        block_t *new_block;
        new_block = get_next_block(block);
        if(new_block == NULL)
        {
            return;
        }
        write_block(new_block, block_size - asize, false);
        add_to_list(new_block);
    }
    else
    {
        remove_from_list(block);
        write_block(block, block_size, true); // renew alloc bit
        
    }
    dbg_requires(mm_checkheap(__LINE__));
}

/*
 * Remove the target block from the free block list.
 */
static void remove_from_list(block_t *target)
{    
    dbg_requires(mm_checkheap(__LINE__));        
    dbg_printf("remove %p\n", target);            

    block_t *tmp_prev = target->content.ptrs.prev;            
    block_t *tmp_next = target->content.ptrs.next;
    tmp_prev->content.ptrs.next = tmp_next;
    tmp_next->content.ptrs.prev = tmp_prev; 
    
    counter_global--;

    dbg_requires(mm_checkheap(__LINE__));    
    return;
}

/*
 * Add the given block new to the free block list.
 */
static void add_to_list(block_t *new)
{
    dbg_printf("add free %p\n", new);

    block_t *old_tail = free_root->content.ptrs.prev;    
    old_tail->content.ptrs.next = new;
    new->content.ptrs.next = free_root;

    free_root->content.ptrs.prev = new;
    new->content.ptrs.prev = old_tail;

    counter_global++;
    dbg_requires(mm_checkheap(__LINE__));
}

/****************** STATIC HELPER METHODS ************************************/

/************ LIST OPERATIONS ************/

/*
 * Get the next free block of curr in the free block list.
 */
static block_t *get_next_free(block_t *curr)
{
    return curr->content.ptrs.next;
}

/*
 * Get the previous free block of curr in the list.
 */
static block_t *get_prev_free(block_t *curr)
{
    return curr->content.ptrs.prev;
}

/************ HEAP OPERATIONS ************/

/*
 * Get the block physically next to the curr block.
 */
static block_t *get_next_block(block_t *curr)
{
    dbg_requires(curr != NULL);    
    block_t *next_block = (block_t *)(((char *)curr) + get_size(curr));
    return next_block;    
}

/*
 * Get the block physically ahead of the curr block.
 */
static block_t *get_prev_block(block_t *curr)
{
    dbg_requires(curr != NULL);
    word_t *footerp = find_prev_footer(curr);
    size_t size = extract_size(*footerp);
    return (block_t *)((char *)curr - size);
}

/************ BLOCK OPERATIONS ************/

/*
 * Return the payload pointer with the given block pointer.
 */
static void *header_to_payload(block_t *block)
{
    return (void *)(block->content.payload);
}

/*
 * Return the header(block) pointer with the given payload pointer.
 */
static block_t *payload_to_header(void *bp)
{
    return (block_t *)(((char *)bp) - offsetof(block_t, content));
}

/*
 * Return the size of the block with the given value.
 */
static size_t extract_size(word_t word)
{
    return (word & size_mask);
}

/*
 * Return the allocation status with the given value.
 */
static bool extract_alloc(word_t word)
{
    return (bool)(word & alloc_mask);
}

/*
 * Return the size of the given block.
 */
static size_t get_size(block_t *block)
{
    return extract_size(block->header);
}

/*
 * Return the allocation status of the given blcok.
 */
static bool get_alloc(block_t *block)
{
    return extract_alloc(block->header);
}

/*
 * Return the footer of the physically previous block.
 */
static word_t *find_prev_footer(block_t *block)
{
    // Compute previous footer position as one word before the header
    return (&(block->header)) - 1;
}

/*
 * Write header and footer of the given blcok with a given size and the given
 * allocation flag.
 */
static void write_block(block_t *block, size_t size, bool is_allocated)
{
    word_t *footerp;
    block->header = pack(size, is_allocated);
    if(block == free_root)
    {
        return;
    }
    else
    {
        footerp = (word_t *)(block->content.payload + get_size(block) - dsize);
    }
    *footerp = pack(size, is_allocated);
}

/************ LOW_LEVEL/MATH OPERATIONS ************/

/*
 * Pack bits of allocation flag and a block size.
 * Return the packed word_t value.
 */
static word_t pack(size_t size, bool is_allocated)
{
    return is_allocated ? (size | alloc_mask) : size;
}

/*
 * Round the given size up to n. Return the rounded value.
 */
static size_t round_up(size_t size, size_t n)
{
    return (n * ((size + (n - 1)) / n));
}

/*
 * Return the maximum of values x and y.
 */
static size_t max(size_t x, size_t y)
{
    return (x > y) ? x : y;
}

/*
 * Return whether an address is in heap. Return true if in heap, else false.
 */
static bool in_heap(const void *p)
{
    return p <= mem_heap_hi() && p >= mem_heap_lo();
}

/*
 * Align the given value to the ALIGNMENT. Return the aligned value.
 */
static size_t align(size_t x)
{
    return ALIGNMENT * ((x + ALIGNMENT - 1) / ALIGNMENT);
}

/*
 * Return whether the pointer is aligned.
 * Return true if the address is aligned, false otherwise.
 */
static bool aligned(const void *p)
{
    size_t ip = (size_t)p;
    return align(ip) == ip;
}

/*
 * Check the correctness and consistency of the heap.
 * May be used in the debug mode.
 */
bool mm_checkheap(int lineno)
{ 
    block_t *lo = mem_heap_lo(); 
    block_t *hi = mem_heap_hi();
    dbg_printf("mm_checkheap: start");
    block_t *curr = (block_t *) (prologue + 5 * wsize);
    block_t *next;

    size_t size;
    word_t hdr;
    word_t ftr;
    if (curr == NULL)
    {
        return true;
    }
    while ((next = get_next_block(curr)) + 1 < hi)
    {
        hdr = curr->header;
        ftr = *find_prev_footer(next);
        bool allocated = get_alloc(curr);
        
        // check header and footer
        if (hdr != ftr)
        {
            printf("[%d] header (0x%016lX) != footer (0x%016lX) at %p\n",
                   lineno, hdr, ftr, curr);
            return false;
        }
        // check payload address alignment
        if (allocated)
        {
            if (!aligned(curr->content.payload))
            {
                printf("[%d] payload (0x%016lX) not aligned at block %p\n",
                       lineno, (word_t) & (curr->content.payload), curr);
                return false;
            }
        }
        // check minimum size
        size = get_size(curr);
        if (size < min_block_size)
        {
            printf("[%d] block %p with header (0x%016lX): its size less than min_block_size.\n",
                   lineno, curr, hdr);
            return false;
        }
        if (size % dsize != 0)
        {
            printf("[%d] block %p with header (0x%016lX): size not dsize.\n",
                   lineno, curr, hdr);
            return false;
        }
        // check coalescing: no adjacent free blocks
        bool curr_alloc = get_alloc(curr);
        bool next_alloc = get_alloc(next);
        if (!curr_alloc || !next_alloc)
        {
            printf("[%d] block %p and next block %p: two adjacent free blocks. (%d and %d)\n",
                   lineno, curr, next, curr_alloc, next_alloc);
            return false;
        }
        curr = next;
    }

    // check free blocks    
    int count = 0;
    for (block_t *tmp = get_next_free(free_root); get_size(tmp) > 0; tmp = get_next_free(tmp))
    {
        // Check address boundary        
        if(tmp < lo || tmp > hi)
        {
            dbg_printf("[%d] block(%p) exceeds heap range(%p, %p)", lineno, tmp, lo, hi);
            return false;
        }
        // Check block number - to debug "add_to_list" and "remove_from_list"
        count++;

        // Check alignment
        if(aligned(tmp->content.payload))
        {
            dbg_printf("[%d] %d free block aligned %p\n", lineno, count, tmp);
        }

        
    }
    dbg_printf("[%d] the length of free :%d  global: %d\n", lineno, count, counter_global);
    dbg_printf("mm_checkheap: end");
    return true;
}
