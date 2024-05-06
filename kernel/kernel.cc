#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"
#include "atomic.hh"

// kernel.cc
//
//    This is the kernel.

// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000 // initial state only

proc ptable[PID_MAX]; // array of process descriptors
                      // Note that `ptable[0]` is never used.
proc *current;        // pointer to currently executing proc

#define HZ 100                      // timer interrupt frequency (interrupts/sec)
static atomic<unsigned long> ticks; // # timer interrupts so far

// Memory state - see `kernel.hh`
physpageinfo physpages[NPAGES];

[[noreturn]] void schedule();
[[noreturn]] void run(proc *p);
void exception(regstate *regs);
uintptr_t syscall(regstate *regs);
void memshow();

int sys_exit(pid_t pid); // doing this for the exception function

// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

static void process_setup(pid_t pid, const char *program_name);

void kernel_start(const char *command)
{
    // initialize hardware
    init_hardware();
    log_printf("Starting WeensyOS\n");

    ticks = 1;
    init_timer(HZ);

    // clear screen
    console_clear();

    // (re-)initialize kernel page table
    for (uintptr_t addr = 0; addr < MEMSIZE_PHYSICAL; addr += PAGESIZE)
    {
        int perm = PTE_P | PTE_W | PTE_U;
        if (addr == 0)
        {
            // nullptr is inaccessible even to the kernel
            perm = 0;
        }
        else if (addr < PROC_START_ADDR && addr != CONSOLE_ADDR)
        {
            perm = PTE_P | PTE_W;
        }
        // install identity mapping
        int temp1 = vmiter(kernel_pagetable, addr).try_map(addr, perm);
        assert(temp1 == 0); // mappings during kernel_start MUST NOT fail
                            // (Note that later mappings might fail!!)
    }

    // set up process descriptors
    for (pid_t i = 0; i < PID_MAX; i++)
    {
        ptable[i].pid = i;
        ptable[i].state = P_FREE;
    }
    if (!command)
    {
        command = WEENSYOS_FIRST_PROCESS;
    }
    if (!program_image(command).empty())
    {
        process_setup(1, command);
    }
    else
    {
        process_setup(1, "allocator");
        process_setup(2, "allocator2");
        process_setup(3, "allocator3");
        process_setup(4, "allocator4");
    }

    // switch to first process using run()
    run(&ptable[1]);
}

// kalloc(sz)
//    Kernel physical memory allocator. Allocates at least `sz` contiguous bytes
//    and returns a pointer to the allocated memory, or `nullptr` on failure.
//    The returned pointer’s address is a valid physical address, but since the
//    WeensyOS kernel uses an identity mapping for virtual memory, it is also a
//    valid virtual address that the kernel can access or modify.
//
//    The allocator selects from physical pages that can be allocated for
//    process use (so not reserved pages or kernel data), and from physical
//    pages that are currently unused (`physpages[N].refcount == 0`).
//
//    On WeensyOS, `kalloc` is a page-based allocator: if `sz > PAGESIZE`
//    the allocation fails; if `sz < PAGESIZE` it allocates a whole page
//    anyway.
//
//    The returned memory is initially filled with 0xCC, which corresponds to
//    the `int3` instruction. Executing that instruction will cause a `PANIC:
//    Unhandled exception 3!`

void *kalloc(size_t sz)
{
    if (sz > PAGESIZE)
    {
        return nullptr;
    }

    int pageno = 0;
    // When the loop starts from page 0, `kalloc` returns the first free page.
    // Alternate search strategies can be faster and/or expose bugs elsewhere.
    // This initialization returns a random free page:
    //     int pageno = rand(0, NPAGES - 1);
    // This initialization remembers the most-recently-allocated page and
    // starts the search from there:
    //     static int pageno = 0;

    for (int tries = 0; tries != NPAGES; ++tries)
    {
        uintptr_t pa = pageno * PAGESIZE;
        if (allocatable_physical_address(pa) && physpages[pageno].refcount == 0)
        {
            ++physpages[pageno].refcount;
            memset((void *)pa, 0xCC, PAGESIZE);
            return (void *)pa;
        }
        pageno = (pageno + 1) % NPAGES;
    }

    return nullptr;
}

// kfree(kptr)
//    Free `kptr`, which must have been previously returned by `kalloc`.
//    If `kptr == nullptr` does nothing.

void kfree(void *kptr)
{
    if (kptr == nullptr)
    {
        return;
    }
    assert(physpages[(uintptr_t)kptr / PAGESIZE].refcount > 0); // check if page needs to be freed, no double free
    assert((uintptr_t)kptr % PAGESIZE == 0);                    // just checking for alignment :)
    physpages[(uintptr_t)kptr / PAGESIZE].refcount--;           // decrement ref count now that its been freed
}

// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char *program_name)
{
    init_process(&ptable[pid], 0);

    // initialize process page table
    // ptable[pid].pagetable = kernel_pagetable;
    ptable[pid].pagetable = kalloc_pagetable();

    // copying from kernel table to new process table allocated above
    vmiter srcit(kernel_pagetable, 0);
    vmiter dstit(ptable[pid].pagetable, 0); // code from section notes, thanks for this :)))))
    for (; srcit.va() < PROC_START_ADDR; srcit += PAGESIZE, dstit += PAGESIZE)
    {
        dstit.map(srcit.pa(), srcit.perm());
    }

    // obtain reference to program image
    // (The program image models the process executable.)
    program_image pgm(program_name);

    // allocate and map process memory as specified in program image
    for (auto seg = pgm.begin(); seg != pgm.end(); ++seg)
    {
        for (uintptr_t a = round_down(seg.va(), PAGESIZE);
             a < seg.va() + seg.size();
             a += PAGESIZE)
        {
            // `a` is the process virtual address for the next code or data page
            // (The handout code requires that the corresponding physical
            // address is currently free.)
            void *ptr = kalloc(PAGESIZE);
            assert(ptr != nullptr);
            int perms = PTE_P | PTE_W | PTE_U;
            if (!seg.writable())
            {
                perms = PTE_P | PTE_U; // if not writeable, we wanna restrict
            }
            vmiter it2(ptable[pid].pagetable, a);
            it2.map(ptr, perms);

            // assert(physpages[a / PAGESIZE].refcount == 0);
            // ++physpages[a / PAGESIZE].refcount;
        }
    }

    // copy instructions and data from program image into process memory
    for (auto seg = pgm.begin(); seg != pgm.end(); ++seg)
    {
        uintptr_t ptr = vmiter(ptable[pid].pagetable, seg.va()).pa(); // per each segment, copying instructions using iter
        memset((void *)ptr, 0, seg.size());
        memcpy((void *)ptr, seg.data(), seg.data_size());
    }

    // mark entry point
    ptable[pid].regs.reg_rip = pgm.entry();
    // uintptr_t entry_page_start = round_down(pgm.entry(), PAGESIZE);
    // vmiter it_e(ptable[pid].pagetable, entry_page_start);
    // int entry = it_e.try_map(it_e.pa(), it_e.perm() | PTE_U);

    // allocate and map stack segment
    // Compute process virtual address for stack page
    uintptr_t stack_addr = MEMSIZE_VIRTUAL - PAGESIZE;

    // The handout code requires that the corresponding physical address
    // is currently free.
    // assert(physpages[stack_addr / PAGESIZE].refcount == 0);
    // ++physpages[stack_addr / PAGESIZE].refcount;

    void *newp = kalloc(PAGESIZE);
    assert(newp != nullptr);                            // chAnGED HEREUIHDAIUWHDAIUHD
    vmiter stack_it(ptable[pid].pagetable, stack_addr); // creating iterator to map newp
    stack_it.map(newp, PTE_P | PTE_W | PTE_U);
    ptable[pid].regs.reg_rsp = stack_addr + PAGESIZE;

    // mark process as runnable
    ptable[pid].state = P_RUNNABLE;
}

// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.

void exception(regstate *regs)
{
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: exception %d at rip %p\n",
                current->pid, regs->reg_intno, regs->reg_rip); */

    // Show the current cursor location and memory state
    // (unless this is a kernel fault).
    console_show_cursor(cursorpos);
    if (regs->reg_intno != INT_PF || (regs->reg_errcode & PTE_U))
    {
        memshow();
    }

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();

    // Actually handle the exception.
    switch (regs->reg_intno)
    {

    case INT_IRQ + IRQ_TIMER:
        ++ticks;
        lapicstate::get().ack();
        schedule();
        break; /* will not be reached */

    case INT_PF:
    {
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        uintptr_t rdaddr = round_down(rdcr2(), PAGESIZE); // rounded addr, goes to the last pagesize aligned address
        // so we dont have any weird accesses since we dont know where addr stands within a page, so we are at the start now!
        vmiter tableau(current->pagetable, rdaddr); // please dont penalize for variable name :)
        if (tableau.perm() & PTE_C)
        {
            if (physpages[tableau.pa() / PAGESIZE].refcount == 1)
            { // HERE WE CHECK IF FORK IS DEAD.
                tableau.map(tableau.pa(), PTE_PWU);
            }
            else
            {
                void *pg = kalloc(PAGESIZE); // allocating new pages based on requirement
                if (pg == nullptr)
                {
                    assert(sys_exit(current->pid) == 0); // cleans up
                }
                memcpy(pg, tableau.kptr(), PAGESIZE);

                --physpages[tableau.pa() / PAGESIZE].refcount;
                int temp2 = tableau.try_map(pg, PTE_PWU);
                if (temp2 != 0)
                {
                    kfree(pg);
                    assert(sys_exit(current->pid) == 0); // cleans up
                }
            }
            break;
        }

        const char *operation = regs->reg_errcode & PTE_W
                                    ? "write"
                                    : "read";
        const char *problem = regs->reg_errcode & PTE_P
                                  ? "protection problem"
                                  : "missing page";

        if (!(regs->reg_errcode & PTE_U))
        {
            proc_panic(current, "Kernel page fault on %p (%s %s, rip=%p)!\n",
                       addr, operation, problem, regs->reg_rip);
        }
        error_printf(CPOS(24, 0), 0x0C00,
                     "Process %d page fault on %p (%s %s, rip=%p)!\n",
                     current->pid, addr, operation, problem, regs->reg_rip);
        current->state = P_FAULTED;
        break;
    }

    default:
        proc_panic(current, "Unhandled exception %d (rip=%p)!\n",
                   regs->reg_intno, regs->reg_rip);
    }

    // Return to the current process (or run something else).
    if (current->state == P_RUNNABLE)
    {
        run(current);
    }
    else
    {
        schedule();
    }
}

int syscall_page_alloc(uintptr_t addr);
int sys_fork();

// sys_exit call cleans up and conducts an exit.
int sys_exit(pid_t pid)
{
    if (pid >= 0 && pid < PID_MAX && (ptable[pid].state != P_FREE))
    {
        x86_64_pagetable *pgtbl = ptable[pid].pagetable;
        for (vmiter it(pgtbl, 0); it.va() < MEMSIZE_VIRTUAL; it += PAGESIZE)
        {
            if (it.user() && it.va() != CONSOLE_ADDR)
            {
                kfree(it.kptr());
            }
        }
        for (ptiter it(pgtbl); !it.done(); it.next())
        {
            kfree(it.kptr());
        }
        kfree(pgtbl);
        ptable[pid].state = P_FREE;
        ptable[pid].pagetable = nullptr;
        return 0;
    }
    return -1;
}

// sys_fork helps fork and create child process
pid_t sys_fork()
{
    // finding free id for the new process
    pid_t new_id = 0;
    for (pid_t k = 1; k < PID_MAX; ++k)
    {
        if (ptable[k].state == P_FREE)
        {
            new_id = k;
            break;
        }
    }
    // invalid id check, as in specifications, cannot be 0
    if (new_id < 1)
    {
        return -1;
    }
    // creating new pagetable
    ptable[new_id].pagetable = kalloc_pagetable();
    if (ptable[new_id].pagetable == nullptr)
    {
        return -1;
    }
    // code from section, copying from parent table to new table
    vmiter srcit(ptable[current->pid].pagetable, 0);
    vmiter dstit(ptable[new_id].pagetable, 0);
    for (; srcit.va() < MEMSIZE_VIRTUAL; srcit += PAGESIZE, dstit += PAGESIZE)
    {
        int perms = PTE_P | PTE_U;
        if ((srcit.writable() || (srcit.perm() & PTE_C)) && srcit.user() && srcit.pa() != CONSOLE_ADDR)
        {                   // added condition to see if parent alr has flag
            perms |= PTE_C; // OR EQUALS, NEW TECH UNLOCKED.
            if (srcit.writable())
            {
                srcit.map(srcit.pa(), perms);
            }
        }
        // if (srcit.writable() && srcit.user() && srcit.pa() != CONSOLE_ADDR) {
        //     void *pg = kalloc(PAGESIZE); //allocating new pages based on requirement
        //     if (pg == nullptr) {
        //         sys_exit(new_id); //cleans up
        //         return -1;
        //     }
        //     memcpy(pg, srcit.kptr(), PAGESIZE);
        //     int temp2 = dstit.try_map(pg, srcit.perm());
        //     if (temp2 != 0) {
        //         kfree(pg);
        //         sys_exit(new_id); //cleans up
        //         return -1;
        //     }
        // }
        if (srcit.user() && srcit.present() && srcit.pa() != CONSOLE_ADDR)
        {
            ++physpages[srcit.pa() / PAGESIZE].refcount; // both processes will share the page, so increase refcount
        }
        if (srcit.present())
        {
            int temp3 = dstit.try_map(srcit.pa(), srcit.perm());
            if (temp3 != 0)
            {
                assert(sys_exit(new_id) == 0);
                return -1;
            }
        }
    }
    ptable[new_id].regs = current->regs;
    ptable[new_id].regs.reg_rax = 0;
    ptable[new_id].state = P_RUNNABLE; // setting status
    return new_id;
}

// syscall(regs)
//    Handle a system call initiated by a `syscall` instruction.
//    The process’s register values at system call time are accessible in
//    `regs`.
//
//    If this function returns with value `V`, then the user process will
//    resume with `V` stored in `%rax` (so the system call effectively
//    returns `V`). Alternately, the kernel can exit this function by
//    calling `schedule()`, perhaps after storing the eventual system call
//    return value in `current->regs.reg_rax`.
//
//    It is only valid to return from this function if
//    `current->state == P_RUNNABLE`.
//
//    Note that hardware interrupts are disabled when the kernel is running.

uintptr_t syscall(regstate *regs)
{
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: syscall %d at rip %p\n",
                  current->pid, regs->reg_rax, regs->reg_rip); */

    // Show the current cursor location and memory state.
    console_show_cursor(cursorpos);
    memshow();

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();

    // Actually handle the exception.
    switch (regs->reg_rax)
    {

    case SYSCALL_PANIC:
        user_panic(current);
        break; // will not be reached

    case SYSCALL_GETPID:
        return current->pid;

    case SYSCALL_YIELD:
        current->regs.reg_rax = 0;
        schedule(); // does not return

    case SYSCALL_PAGE_ALLOC:
        return syscall_page_alloc(current->regs.reg_rdi);

    case SYSCALL_FORK:
        return sys_fork();

    case SYSCALL_EXIT:
        assert(sys_exit(current->pid) == 0);
        schedule();

    case SYSCALL_KILL:
        return sys_exit(current->regs.reg_rdi); // takes input reg value of process to kill

    default:
        proc_panic(current, "Unhandled system call %ld (pid=%d, rip=%p)!\n",
                   regs->reg_rax, current->pid, regs->reg_rip);
    }

    panic("Should not get here!\n");
}

// syscall_page_alloc(addr)
//    Handles the SYSCALL_PAGE_ALLOC system call. This function
//    should implement the specification for `sys_page_alloc`
//    in `u-lib.hh` (but in the handout code, it does not).

int syscall_page_alloc(uintptr_t addr)
{
    if ((addr & PAGEOFFMASK) != 0 || addr < PROC_START_ADDR || addr >= MEMSIZE_VIRTUAL)
    {
        return -1;
    }
    void *ptr = kalloc(PAGESIZE);
    if (ptr == nullptr)
    {
        return -1;
    }
    vmiter it(current->pagetable, addr);
    // assert(physpages[addr / PAGESIZE].refcount == 0);
    // ++physpages[addr / PAGESIZE].refcount;
    int temp4 = it.try_map(ptr, PTE_P | PTE_U | PTE_W);
    assert(temp4 == 0);
    memset(ptr, 0, PAGESIZE);
    return 0;
}

// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.

void schedule()
{
    pid_t pid = current->pid;
    for (unsigned spins = 1; true; ++spins)
    {
        pid = (pid + 1) % PID_MAX;
        if (ptable[pid].state == P_RUNNABLE)
        {
            run(&ptable[pid]);
        }

        // If Control-C was typed, exit the virtual machine.
        check_keyboard();

        // If spinning forever, show the memviewer.
        if (spins % (1 << 12) == 0)
        {
            memshow();
            log_printf("%u\n", spins);
        }
    }
}

// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.

void run(proc *p)
{
    assert(p->state == P_RUNNABLE);
    current = p;

    // Check the process's current pagetable.
    check_pagetable(p->pagetable);

    // This function is defined in k-exception.S. It restores the process's
    // registers then jumps back to user mode.
    exception_return(p);

    // should never get here
    while (true)
    {
    }
}

// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

void memshow()
{
    static unsigned last_ticks = 0;
    static int showing = 0;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2)
    {
        last_ticks = ticks;
        showing = (showing + 1) % PID_MAX;
    }

    proc *p = nullptr;
    for (int search = 0; !p && search < PID_MAX; ++search)
    {
        if (ptable[showing].state != P_FREE && ptable[showing].pagetable)
        {
            p = &ptable[showing];
        }
        else
        {
            showing = (showing + 1) % PID_MAX;
        }
    }

    console_memviewer(p);
    if (!p)
    {
        console_printf(CPOS(10, 26), 0x0F00, "   VIRTUAL ADDRESS SPACE\n"
                                             "                          [All processes have exited]\n"
                                             "\n\n\n\n\n\n\n\n\n\n\n");
    }
}
