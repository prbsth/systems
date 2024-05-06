#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <vector>
// Check for realloc: IN THIS TEST, WE CHECK IF THE NUMBER OF BYTES BEING REALLOCED IS SMALLER

int main() {

    char*  ptr = (char*)m61_malloc(10);
    assert(ptr);
    
    char* new_ptr = (char*)m61_realloc(ptr,2);
    assert(new_ptr);
    assert(new_ptr == ptr); //since the pointer should remain the same.

    m61_free(new_ptr);

    m61_print_statistics();

    return 0; 
}

//! alloc count: active          0   total          2   fail        ???
//! alloc size:  active          0   total          2   fail        ???