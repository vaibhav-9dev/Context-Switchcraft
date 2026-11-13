# ContextChaos: Architecture & Design Specification

## 1. Stack Layout & Memory Management
The fiber runtime utilizes two distinct stack management strategies based on the `stack_mode` attribute:

*   **Own Stack (`STACK_OWN`):** Fibers are allocated a private stack (default 64 KiB) via anonymous `mmap`. We allocate an additional 4096 bytes (1 page) and use `mprotect(..., PROT_NONE)` to act as a guard page at the bottom of the stack. This instantly catches stack overflows natively via `SIGSEGV` interception.
*   **Shared Stack (`STACK_SHARED`):** To optimize memory for tens of thousands of idle fibers, the runtime uses a single 256 KiB shared stack pool. When a shared-stack fiber is preempted or yields, the active portion of its stack is copied to a dynamically sized heap buffer (`malloc`/`realloc`). When the fiber is rescheduled, its saved stack is copied back onto the shared stack top. 
*   **AMD64 Red Zone Preservation:** During shared stack copying, the scheduler subtracts 128 bytes from the current `%rsp` before copying to ensure the AMD64 ABI "Red Zone" is preserved, preventing `libc` local variable corruption during preemption.
*   **ABI 16-Byte Alignment:** All stacks push a dummy return address (`0x0`) upon initialization to guarantee that `%rsp` is strictly 16-byte aligned precisely before the fiber's entry function is called.

## 2. Register Save and Restore (Context Switching)
The core context switch is implemented in x86-64 assembly (`context_switch.s`) to bypass the massive overhead of standard `swapcontext`.

*   **Callee-Saved Registers:** The assembly routine pushes the 6 callee-saved registers mandated by the System V ABI (`%rbx`, `%rbp`, `%r12`, `%r13`, `%r14`, `%r15`) onto the current fiber's stack.
*   **Stack Pointer (`%rsp`):** The current stack pointer is then saved into the memory location provided by the first argument (`old_rsp`).
*   **Restoration:** The stack pointer is updated to the second argument (`new_rsp`). The 6 callee-saved registers are popped off this new stack.
*   **Instruction Pointer (`%rip`):** The `ret` instruction seamlessly pops the return address off the top of the newly restored stack and jumps execution to the restored instruction pointer.

## 3. Preemption and FPU/AVX Signal Safety
The scheduler achieves robust, time-sliced preemption using `SIGALRM` and `timer_create` driven by the `CLOCK_PROCESS_CPUTIME_ID` clock.

*   **FPU/AVX State:** We do not manually push XMM/YMM registers in `context_switch.s`. Because preemption is driven by Linux signals, the kernel automatically saves the entire FPU and AVX state (via `xsave`) into the `ucontext_t` signal frame on the fiber's stack when the interrupt occurs. When the signal handler returns (or is later resumed), the kernel perfectly restores the AVX state. 
*   **Signal Safety & Re-entrancy:** To prevent structural corruption of the scheduler's internal circular doubly-linked run-queue, we utilize a `preempt_disable_count` and an `in_scheduler` flag. If the timer fires while the scheduler is actively mutating pointers or holding internal locks, the preemption is deferred (`pending_preemption = 1`) and executed immediately upon exiting the critical section.
*   **libc Compatibility:** Standard library functions (like `malloc` and `printf`) rely on thread-local variables like `errno`. During a context switch, the runtime saves the pre-switch `errno` and restores it post-switch, ensuring seamless `libc` interoperability even during aggressive preemption.

## 4. Polling and Deadlock Prevention
*   **Asynchronous I/O:** Blocking operations (`fiber_read`) are intercepted. The file descriptor is registered via `epoll_ctl` (EPOLLIN | EPOLLONESHOT), and the fiber is parked. The `fiber_yield` loop queries `epoll_wait` with a timeout of `0` (or `-1` if all fibers are blocked), restoring fibers to the run-queue instantly when I/O is ready.
*   **Mutex Cycle Detection:** The runtime uses a unified dependency tracker (`block_type`, `block_target`). Deadlocks are detected in $O(N)$ time by traversing ownership chains for both Mutex waits and `fiber_join` calls before granting a lock.
*   **Priority Unlocking:** Mutexes strictly adhere to R2 scheduling policies. Waiters are evaluated upon unlock, and the lock is yielded to the waiting fiber with the highest priority (lowest priority value), using the lowest Fiber ID as a tie-breaker.