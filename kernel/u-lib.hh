#ifndef WEENSYOS_U_LIB_HH
#define WEENSYOS_U_LIB_HH
#include "lib.hh"
#include "x86-64.h"
#if WEENSYOS_KERNEL
#error "u-lib.hh should not be used by kernel code."
#endif

// u-lib.hh
//
//    Support code for WeensyOS user-level code.


// make_syscall
//    These functions define the WeensyOS system call calling convention.
//    We provide versions for system calls with 0-2 arguments.

__always_inline uintptr_t make_syscall(int syscallno) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax)
            : /* all input registers are also output registers */
            : "cc", "memory", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
              "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0)
            :
            : "cc", "memory", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0,
                                       uintptr_t arg1) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0), "+S" (arg1)
            :
            : "cc", "memory", "rcx", "rdx", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0,
                                       uintptr_t arg1, uintptr_t arg2) {
    register uintptr_t rax asm("rax") = syscallno;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0), "+S" (arg1), "+d" (arg2)
            :
            : "cc", "rcx", "r8", "r9", "r10", "r11");
    return rax;
}

__always_inline uintptr_t make_syscall(int syscallno, uintptr_t arg0,
                                       uintptr_t arg1, uintptr_t arg2,
                                       uintptr_t arg3) {
    register uintptr_t rax asm("rax") = syscallno;
    register uintptr_t r10 asm("r10") = arg3;
    asm volatile ("syscall"
            : "+a" (rax), "+D" (arg0), "+S" (arg1), "+d" (arg2), "+r" (r10)
            :
            : "cc", "rcx", "r8", "r9", "r11");
    return rax;
}

__always_inline void clobber_memory(void* ptr) {
    asm volatile ("" : "+m" (*(char*) ptr));
}

__always_inline void access_memory(const void* ptr) {
    asm volatile ("" : : "m" (*(const char*) ptr));
}


// sys_getpid
//    Return current process ID. (Never fails.)
inline pid_t sys_getpid() {
    return make_syscall(SYSCALL_GETPID);
}

// sys_yield
//    Yield control of the CPU to the kernel. The kernel will pick another
//    process to run, if possible; if there is no other process, it will
//    run this process again. (Never fails.)
inline void sys_yield() {
    make_syscall(SYSCALL_YIELD);
}

// sys_page_alloc(addr)
//    Allocate a page of memory at address `addr` for this process. The
//    newly-allocated memory is initialized to 0. Any memory previously
//    located at `addr` should be freed. Returns 0 on success. If there is a
//    failure (out of memory or invalid argument), returns a negative error
//.   code (such as -1) without modifying memory.
//
//    `Addr` should be page-aligned (i.e., a multiple of PAGESIZE == 4096),
//    >= PROC_START_ADDR, and < MEMSIZE_VIRTUAL. If any of these requirements
//    are not met, returns a negative error code without modifying memory.
inline int sys_page_alloc(void* addr) {
    return make_syscall(SYSCALL_PAGE_ALLOC, (uintptr_t) addr);
}

// sys_fork()
//    Fork the current process. On success, returns the child's process ID to
//    the parent, and returns 0 to the child. On failure, returns a negative
//    error code without creating a new process.
inline pid_t sys_fork() {
    return make_syscall(SYSCALL_FORK);
}

//sys_kill() also new
inline pid_t sys_kill(pid_t pid) {
    return make_syscall(SYSCALL_KILL, pid);
}

// sys_exit()
//    Exit this process. Does not return.
[[noreturn]] inline void sys_exit() {
    make_syscall(SYSCALL_EXIT);
    make_syscall(SYSCALL_PANIC, (uintptr_t) "sys_exit should not return!");

    // should never get here
    while (true) {
    }
}

// sys_panic(msg)
//    Panic.
[[noreturn]] inline void sys_panic(const char* msg) {
    make_syscall(SYSCALL_PANIC, (uintptr_t) msg);

    // should never get here
    while (true) {
    }
}

#endif