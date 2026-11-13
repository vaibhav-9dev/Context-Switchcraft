# ContextChaos: Fiber Runtime Architecture Specification

## 1. Overview
ContextChaos is a high-performance, user-space fiber runtime designed for x86-64 Linux systems. It supports cooperative multitasking, real-time timer-based preemption, asynchronous I/O polling via `epoll`, shared run stacks, and deadlock-detecting mutexes (R2 policy).

## 2. Context Switching & Register Save/Restore
The core context switch is implemented entirely in x86-64 assembly to avoid the overhead of system calls (like `rt_sigprocmask` used by standard `swapcontext`).

*   **Integer Registers:** The runtime adheres to the System V AMD64 ABI. The `fiber_switch` assembly routine pushes the six callee-saved registers (`%rbp`, `%rbx`, `%r12`, `%r13`, `%r14`, `%r15`) onto the outgoing fiber's stack. 
*   **Stack Pointer (`%rsp`):** The current stack pointer is saved into the outgoing fiber's context block, and the incoming fiber's stack pointer is loaded into `%rsp`.
*   **Instruction Pointer (`%rip`):** The instruction pointer is implicitly saved and restored via the standard `call` and `ret` instructions. When `fiber_switch` executes `ret`, it pops the incoming fiber's saved `%rip` off its stack, seamlessly resuming execution.

## 3. Preemption and Signal Safety (FPU/AVX State)
When `FIBER_CLOCK=real` is enabled, the runtime utilizes `CLOCK_PROCESS_CPUTIME_ID` to fire a `SIGALRM` every `FIBER_QUANTUM_US` microseconds. 

*   **FPU & AVX Preservation:** Saving and restoring 256-bit YMM or 512-bit ZMM registers manually in user space is expensive and architecturally brittle. We delegate this to the Linux kernel. When the timer fires, the kernel interrupts the fiber and saves its exact CPU state (including FPU, MXCSR, and AVX registers) onto a signal frame on the fiber's stack. 
*   **Yielding from the Handler:** The `SIGALRM` handler increments the preemption counter and calls `fiber_yield()`. The assembly context switch only swaps integer registers. When the preempted fiber is eventually scheduled back in, it resumes execution inside the signal handler. When the handler returns, the kernel's `sigreturn` routine automatically restores the heavy FPU/AVX state exactly as it was before the interruption.
*   **Signal Masking:** The timer is registered with `SA_NODEFER` to ensure nested preemptions do not permanently mask `SIGALRM`. `fiber_preempt_disable()` defers preemption by incrementing a local counter; if a signal fires while disabled, it is flagged and actioned upon `fiber_preempt_enable()`.

## 4. Stack Layout and Memory Management
The runtime supports two stack modes to balance isolation and memory efficiency: `STACK_OWN` and `STACK_SHARED`.

### 4.1. STACK_OWN
Each fiber receives a dedicated stack allocated via `mmap` (default 64 KiB). 
*   **Overflow Protection:** A 4 KiB guard page (`PROT_NONE`) is mapped immediately below the stack base. A custom `SIGSEGV` handler, running on an alternate signal stack (`sigaltstack`), catches guard page faults, prints the faulting fiber's ID, and cleanly aborts the process (satisfying the S5 invariant).

### 4.2. STACK_SHARED (Memory Multiplexing)
To support 50,000+ parked fibers with a peak RSS under 128 MiB, `STACK_SHARED` fibers share a single, global 256 KiB execution stack.
*   **The Scheduler Trampoline:** Direct context switching between shared fibers is dangerous because pushing registers during the switch could corrupt the incoming fiber's data. To solve this, yielding shared fibers bounce through a dedicated 64 KiB **Scheduler Trampoline Stack**.
*   **Live Stack Backup:** On the trampoline, the runtime calculates the exact active footprint of the outgoing fiber (Global Stack Top - current `%rsp`), dynamically sizes a heap buffer, and `memcpy`s the live bytes off the shared stack. It then copies the incoming fiber's saved bytes back onto the shared stack before jumping into it.

## 5. Asynchronous I/O (`fiber_read`)
To prevent a blocking `read()` call from freezing the entire OS thread, `fiber_read` integrates directly with Linux `epoll`. 
*   When a fiber reads from a pipe or timerfd, the file descriptor is added to an `epoll` instance with `EPOLLONESHOT`, and the fiber is marked as `FIBER_BLOCKED`.
*   The `fiber_yield()` function checks `epoll_wait` on every invocation. If the run queue is empty, it sleeps indefinitely until I/O arrives; otherwise, it polls instantly (`timeout = 0`). Awoken fibers are seamlessly placed back at the tail of the round-robin queue.

## 6. Mutex Synchronization (R2 Policy)
The custom `fmutex_t` implements strict cycle detection and priority hand-off to prevent barging and deadlocks.
*   **Cycle Detection:** On `fmutex_lock`, the runtime traverses a linked list of `waiting_on` pointers. If it encounters a cycle (e.g., A waits on B, B waits on A), it instantly rejects the lock with `EDEADLK`.
*   **Priority Handoff:** On `fmutex_unlock`, ownership is transferred directly to the waiting fiber with the highest accumulated `lock_hold_ticks`. Ties are broken by granting the lock to the fiber with the lowest ID.