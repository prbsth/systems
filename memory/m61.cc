#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>
#include <map>
#include <iostream>

// New variable initializations
static m61_statistics gstats = {
    .nactive = 0,
    .active_size = 0,
    .ntotal = 0,
    .total_size = 0,
    .nfail = 0,
    .fail_size = 0,
    .heap_min = UINTPTR_MAX,
    .heap_max = 0
};
//struct to hold information about each entry into malloc
struct properties {
    size_t actual_size;
    size_t padding_size;
    const char* file;
    int line;
};

std::map<void*, size_t> freed_addresses;
std::map<void*, properties> active_addresses;
int highest_free = 0;

struct m61_memory_buffer {
    char* buffer;
    size_t pos = 0;
    size_t size = 8 << 20; /* 8 MiB */

    m61_memory_buffer();
    ~m61_memory_buffer();
};

static m61_memory_buffer default_buffer;


m61_memory_buffer::m61_memory_buffer() {
    void* buf = mmap(nullptr,    // Place the buffer at a random address
        this->size,              // Buffer should be 8 MiB big
        PROT_WRITE,              // We want to read and write the buffer
        MAP_ANON | MAP_PRIVATE, -1, 0);
                                 // We want memory freshly allocated by the OS
    assert(buf != MAP_FAILED);
    this->buffer = (char*) buf;
    freed_addresses.insert ({(void*)buffer, size});
}

m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}

//getting value of first pointer in the heap
void* first_heap = freed_addresses.begin()->first;

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings

    //checking if sz is larger than buffer can handle
    if (sz == SIZE_MAX || sz >= default_buffer.size) {
        ++gstats.nfail;
        gstats.fail_size += sz;
        return nullptr;
    }

    size_t alignment = alignof(std::max_align_t);
    void* fptr = nullptr; //inital assignment of fptr, will get changed if suitable address is found, otherwise will be returned as null
    size_t padding = (alignment - (sz % alignment));
    if (padding == 0) { //if no more space to add magic footer to store value for checking buffer overwrite
        padding = (alignment - ((sz+1) % alignment));
    }
    for (auto it = freed_addresses.begin(); it!=freed_addresses.end(); ++it){
        if (it->second >= (sz + padding)) {
            fptr =  it->first;
            size_t o_size = it->second;
            size_t size_left = o_size - (sz + padding);
            if (size_left > 0) {
                void* nptr = (void*)((uintptr_t)fptr + sz + padding);
                freed_addresses.insert ({nptr,size_left});
            }
            freed_addresses.erase(it);
            
            active_addresses.insert ({fptr, {sz, padding, file, line}});
            char* magicptr = (char*)((uintptr_t)fptr + sz);
            *magicptr = 61;
            break;
        }
    }

    if (fptr != nullptr) {
        //statistics
        ++gstats.ntotal;
        if (gstats.ntotal == 1) { //if first use of heap, then set min
            gstats.heap_min = (uintptr_t) fptr;}
        if (gstats.heap_min > (uintptr_t) fptr) {
            gstats.heap_min = (uintptr_t) fptr;}
        if (gstats.heap_max < ((uintptr_t) fptr) + sz) {
            gstats.heap_max = (uintptr_t) (fptr) + sz;
        }
        ++gstats.nactive;
        gstats.total_size += sz;
        gstats.active_size += sz;
        //assertion just to make sure
        assert(fptr != nullptr);
        return fptr;
    }
    ++gstats.nfail;
    gstats.fail_size += sz;
    return nullptr;
}

//THE DESCRIPTION FROM PSET DESCRIPTION
/// m61_realloc(ptr, sz, file, line)
///    Changes the size of the dynamic allocation pointed to by `ptr`
///    to hold at least `sz` bytes. If the existing allocation cannot be
///    enlarged, this function makes a new allocation, copies as much data
///    as possible from the old allocation to the new, and returns a pointer
///    to the new allocation. If `ptr` is `nullptr`, behaves like
///    `m61_malloc(sz, file, line). `sz` must not be 0. If a required
///    allocation fails, returns `nullptr` without freeing the original
///    block.

void* m61_realloc(void* ptr, size_t sz, const char* file, int line)
{
    (void)file, (void)line; 
    if (sz == 0) { //nothing to realloc but we free
        m61_free(ptr, file, line);
        return nullptr;
    }
    if (ptr == nullptr) { //just mallocing in this case.
        return m61_malloc(sz, file, line);
    }
    auto it = active_addresses.find(ptr);
    if (it != active_addresses.end()) {
        size_t old_size = it->second.actual_size + it->second.padding_size;
        size_t alignment = alignof(std::max_align_t);
        size_t padding = (alignment - (sz % alignment)); //same as what i did for malloc
        if (padding == 0) { //if no more space to add magic footer to store value for checking buffer overwrite
            padding = (alignment - ((sz+1) % alignment));
        }
        size_t new_aligned_sz = sz + padding; //new size with everything included
        if (new_aligned_sz <= old_size) {
            it->second.actual_size = sz;
            it->second.padding_size = padding;
            // if extra space, add to free addr.
            size_t diff = old_size - new_aligned_sz;
            if (diff > 0) {
                void* newptr = (void*)((char*)ptr + new_aligned_sz);
                freed_addresses.insert ({newptr, diff});
                char* magicptr = (char*)((uintptr_t)ptr + sz);
                *magicptr = 61; //setting the magicptr at new location IF THE NEW ALLOCATION IS SMALLER
            }
            //so in this case, new active size is actually lesser
            gstats.active_size += (new_aligned_sz - old_size);
            return ptr;
        }

        // making it bigger, so we have to basically just treat it as a new malloc
        else {
            // mallocing
            void* nptr = m61_malloc(sz, file, line);
            if (nptr) {
                m61_free(ptr, file, line); //we free the original thing
                memcpy(nptr, ptr, sz);
            }
            return nptr;
        }
    } else {
        // ayy, cannae reallocate sire. 
        fprintf(stderr, "MEMORY BUG: %s:%d: invalid realloc of pointer %p, not allocated\n", file, line, ptr);
        abort();
        return nullptr; 
    }
}



/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.

void m61_free(void* ptr, const char* file, int line) {
    // avoid uninitialized variable warnings
    (void) ptr, (void) file, (void) line;
    //check if ptr passed as arg is null, if so return empty
    if (ptr == nullptr) {return;}
    //check if ptr passed as arg is already freed, if so return empty
    if (freed_addresses.find(ptr) != freed_addresses.end()) {
    std::cerr << "MEMORY BUG: invalid free of pointer "<< ptr <<", double free\n";
    abort();
    }
    //checking invalid free on non-heap pointer
    if (ptr < first_heap || (uintptr_t)ptr > ((uintptr_t) first_heap + default_buffer.size)){
    std::cerr << "MEMORY BUG: invalid free of pointer "<< ptr <<", not in heap\n";
    abort();
    }
    //check if ptr passed as arg is part of active_addresses map, if 
    auto it = active_addresses.find(ptr);
    if (it == active_addresses.end()) {
        auto it2 = active_addresses.lower_bound(ptr); 
        it2--; //puts it2 iterator to the closest lower value to ptr in the map, if such a value exists
        std::cerr << "MEMORY BUG: "<<file<<":"<<line<<": invalid free of pointer "<< ptr <<", not allocated\n";
        if (ptr > it2->first && ptr < (void*)((uintptr_t)it2->first + it2->second.actual_size+it2->second.padding_size)){
            std::cerr <<file<<":"<<it2->second.line<<": "<< ptr <<" is "<< ((uintptr_t)ptr - (uintptr_t)it2->first) <<" bytes inside a "<<it2->second.actual_size<<" byte region allocated here\n";
        }
        abort();
    }

    //checking for boundary write error
    char* magicptrcheck = (char*) ((uintptr_t) ptr + it->second.actual_size);
    if (*magicptrcheck != 61){
        std::cerr << "MEMORY BUG: "<<file<<":"<<line<<": detected wild write during free of pointer "<< ptr <<"\n";
        abort();
    }

    //adding the ptr to freed addresses map after coalescing
    size_t total_free_size = (it -> second.actual_size) + (it -> second.padding_size);
    
    //assertion just to make sure
    assert (ptr!=nullptr);

    //Coalesce down
    auto prev_block = freed_addresses.lower_bound(ptr);
    if (prev_block != freed_addresses.begin()){  
        --prev_block;
        if (prev_block != freed_addresses.begin() && ((uintptr_t) prev_block->first == (uintptr_t) ptr - prev_block->second)) {
                total_free_size += prev_block->second;
                ptr = prev_block->first;
                --prev_block;
                freed_addresses.erase(ptr);
        }
    }

    //Coalesce up
    auto next_block = freed_addresses.upper_bound(ptr);
    while (next_block != freed_addresses.begin() && next_block != freed_addresses.end() && (void*) next_block->first == (void*) ((uintptr_t)ptr + total_free_size)) 
        {
            size_t size_added = next_block->second;
            ++next_block;
            freed_addresses.erase((void*) ((uintptr_t) ptr + total_free_size));
            total_free_size += size_added;
    }

    //Statistics
    --gstats.nactive;
    gstats.active_size -= it -> second.actual_size;
    freed_addresses.insert ({ptr, total_free_size});
    active_addresses.erase (it);

    
}


/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.

void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    if (count > SIZE_MAX / sz || (sz*count) >= default_buffer.size) {
        ++gstats.nfail;
        gstats.fail_size += sz;
        return nullptr;
    }
    void* ptr = m61_malloc(count * sz, file, line);
    if (ptr) {
        memset(ptr, 0, count * sz);
    }
    return ptr;
}


/// m61_get_statistics()
///    Return the current memory statistics.

m61_statistics m61_get_statistics() {
    return gstats;
}


/// m61_print_statistics()
///    Prints the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats = m61_get_statistics();
    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Prints a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    auto it = active_addresses.begin(); //iterating through the whole active addresses map and printing
    while (it != active_addresses.end()) {
         fprintf(stdout, "LEAK CHECK: %s:%d: allocated object %p with size %zu\n", it->second.file, it->second.line, it->first, it->second.actual_size);
        ++it;
    }
}

