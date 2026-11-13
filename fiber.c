#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "include/fiber.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <ucontext.h>

extern void fiber_switch(void **old_rsp, void *new_rsp);

#define BLOCK_NONE  0
#define BLOCK_MUTEX 1
#define BLOCK_JOIN  2

typedef struct fiber_node {
    fiber_stat_t stat;
    void *rsp;
    void *retval;
    struct fiber_node *joiner;
    
    int slice_remaining;
    
    int block_type;
    void *block_target; 
    
    struct fiber_node *next_waiter;
    int io_fd;
    
    void *saved_stack;
    void *stack_base;
    size_t stack_size;
    
    void (*fn)(void *); 
    void *arg;          
    
    struct fiber_node *run_next;
    struct fiber_node *run_prev;
    int is_in_runqueue;
    int saved_errno;

    int preempt_disable_count;
    int pending_preemption;

    uint16_t fpu_cw;
    uint32_t mxcsr;
} fiber_node_t;

typedef struct {
    fiber_node_t *owner;
    fiber_node_t *waiters_head;
    uint64_t acquired_at_tick;
} mutex_internal_t;

_Static_assert(sizeof(mutex_internal_t) <= 64, "mutex_internal_t exceeds 64 bytes");

static fiber_node_t *current = NULL;
static fiber_node_t *run_list = NULL;
static fiber_t next_id = 1;

static int clock_mode_real = 0;
static timer_t global_timerid;

static int epoll_fd = -1;
static int io_waitcount = 0;

static fiber_node_t **fibers = NULL;
static size_t fibers_cap = 0;

static void *shared_stack_base = NULL;
static void *shared_stack_top = NULL;
static void *scheduler_rsp = NULL;
static fiber_node_t *sched_prev = NULL;
static fiber_node_t *sched_next = NULL;

static uint16_t default_fpu_cw = 0x037f;
static uint32_t default_mxcsr = 0x1f80;

#define MAX_FDS 65536
static fiber_node_t *fd_waiters[MAX_FDS] = {0};

#define API_ENTER(oldset) \
    sigset_t oldset; \
    do { \
        if (clock_mode_real) { \
            sigset_t set; sigemptyset(&set); sigaddset(&set, SIGALRM); \
            sigprocmask(SIG_BLOCK, &set, &(oldset)); \
        } \
    } while(0)

#define API_LEAVE(oldset) \
    do { \
        if (clock_mode_real) { \
            sigprocmask(SIG_SETMASK, &(oldset), NULL); \
        } \
    } while(0)

/* ---- Strict memory bounds for safe libc preemption ---- */
typedef struct {
    uintptr_t start;
    uintptr_t end;
} mem_region_t;

#define MAX_CRIT_REGIONS 64
static mem_region_t crit_regions[MAX_CRIT_REGIONS];
static int num_crit_regions = 0;

static void init_crit_bounds(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s, e;
        char path[256] = {0};
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %255s", &s, &e, path) >= 2) {
            /* Safely match standard glibc paths without matching "libcontextchaos" */
            if (strstr(path, "/libc.so") || strstr(path, "/libc-") || strstr(path, "/ld-")) {
                if (num_crit_regions < MAX_CRIT_REGIONS) {
                    crit_regions[num_crit_regions].start = s;
                    crit_regions[num_crit_regions].end = e;
                    num_crit_regions++;
                }
            }
        }
    }
    fclose(f);
}
/* ------------------------------------------------------- */

static void register_fiber(fiber_node_t *node) {
    if (node->stat.id >= (fiber_t)fibers_cap) {
        size_t new_cap = fibers_cap == 0 ? 16 : fibers_cap * 2;
        while (node->stat.id >= (fiber_t)new_cap) new_cap *= 2;
        fibers = realloc(fibers, new_cap * sizeof(fiber_node_t *));
        for (size_t i = fibers_cap; i < new_cap; i++) fibers[i] = NULL;
        fibers_cap = new_cap;
    }
    fibers[node->stat.id] = node;
}

static void add_runnable(fiber_node_t *node) {
    if (node->is_in_runqueue) return;
    node->stat.state = FIBER_RUNNABLE;
    node->is_in_runqueue = 1;

    if (!run_list) {
        run_list = node;
        node->run_next = node;
        node->run_prev = node;
        return;
    }

    if (node->stat.id < run_list->stat.id) {
        fiber_node_t *tail = run_list->run_prev;
        node->run_next = run_list;
        node->run_prev = tail;
        tail->run_next = node;
        run_list->run_prev = node;
        run_list = node;
    } else {
        fiber_node_t *curr = run_list->run_next;
        while (curr != run_list && curr->stat.id < node->stat.id) {
            curr = curr->run_next;
        }
        fiber_node_t *prev = curr->run_prev;
        node->run_next = curr;
        node->run_prev = prev;
        prev->run_next = node;
        curr->run_prev = node;
    }
}

static void remove_runnable(fiber_node_t *node) {
    if (!node->is_in_runqueue) return;
    node->is_in_runqueue = 0;
    if (node->run_next == node) {
        run_list = NULL;
    } else {
        node->run_prev->run_next = node->run_next;
        node->run_next->run_prev = node->run_prev;
        if (run_list == node) run_list = node->run_next;
    }
}

static struct sigaction old_segv_sa;

static void segv_handler(int sig, siginfo_t *si, void *uc) {
    void *addr = si->si_addr;
    int is_overflow = 0;
    
    if (current && current->stat.stack_mode == STACK_SHARED) {
        if (addr >= shared_stack_base && addr < (void*)((char*)shared_stack_base + 4096)) is_overflow = 1;
    } else if (current && current->stat.stack_mode == STACK_OWN) {
        if (addr >= current->stack_base && addr < (void*)((char*)current->stack_base + 4096)) is_overflow = 1;
    }
    
    if (is_overflow) {
        fprintf(stderr, "contextchaos: stack overflow in fiber %lld\n", (long long)current->stat.id);
        abort();
    }
    
    if (old_segv_sa.sa_sigaction) {
        old_segv_sa.sa_sigaction(sig, si, uc);
    } else if (old_segv_sa.sa_handler == SIG_DFL) {
        signal(SIGSEGV, SIG_DFL);
        raise(SIGSEGV);
    }
}

static void scheduler_loop(void) {
    while (1) {
        fiber_node_t *p = sched_prev;
        fiber_node_t *n = sched_next;
        
        if (p && p->stat.stack_mode == STACK_SHARED) {
            size_t used = (char*)shared_stack_top - (char*)p->rsp;
            if (used > p->stat.max_saved_stack_bytes) {
                p->stat.max_saved_stack_bytes = used;
                p->saved_stack = realloc(p->saved_stack, used);
            }
            memcpy(p->saved_stack, p->rsp, used);
            p->stat.saved_stack_bytes = used;
        }
        
        if (n && n->stat.stack_mode == STACK_SHARED) {
            size_t used = n->stat.saved_stack_bytes;
            if (used > 0) {
                void *dest = (char*)shared_stack_top - used;
                memcpy(dest, n->saved_stack, used);
            }
        }
        
        fiber_switch(&scheduler_rsp, n->rsp);
    }
}

static void timer_handler(int sig, siginfo_t *si, void *uc_ptr) {
    (void)sig;
    (void)si;
    int saved_err = errno;
    
    ucontext_t *uc = (ucontext_t *)uc_ptr;
    uintptr_t rip = uc->uc_mcontext.gregs[REG_RIP];
    
    int ticks_to_add = 1;
    if (clock_mode_real) {
        int overruns = timer_getoverrun(global_timerid);
        if (overruns > 0) {
            ticks_to_add += overruns;
        }
    }
    
    if (current) {
        current->stat.ticks += ticks_to_add;
        current->slice_remaining -= ticks_to_add;
        
        if (current->slice_remaining <= 0) {
            int in_crit = 0;
            for (int i = 0; i < num_crit_regions; i++) {
                if (rip >= crit_regions[i].start && rip < crit_regions[i].end) {
                    in_crit = 1;
                    break;
                }
            }
            
            if (current->preempt_disable_count > 0 || in_crit) {
                current->pending_preemption = 1;
            } else {
                current->stat.preemptions++;
                fiber_yield();
            }
        }
    }
    errno = saved_err;
}

static void ensure_init(void) {
    if (!current) {
        __asm__ volatile("fnstcw %0" : "=m"(default_fpu_cw));
        __asm__ volatile("stmxcsr %0" : "=m"(default_mxcsr));

        if (epoll_fd < 0) epoll_fd = epoll_create1(0);

        stack_t ss;
        ss.ss_sp = malloc(SIGSTKSZ);
        ss.ss_size = SIGSTKSZ;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);

        struct sigaction sa_segv;
        memset(&sa_segv, 0, sizeof(sa_segv));
        sa_segv.sa_sigaction = segv_handler;
        sa_segv.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa_segv, &old_segv_sa);

        shared_stack_base = mmap(NULL, 256 * 1024 + 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        mprotect(shared_stack_base, 4096, PROT_NONE);
        shared_stack_top = (char*)shared_stack_base + 256 * 1024 + 4096;

        void *sched_stack = mmap(NULL, 65536 + 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        mprotect(sched_stack, 4096, PROT_NONE);
        uintptr_t *sp = (uintptr_t*)((char*)sched_stack + 65536 + 4096);
        
        sp = (uintptr_t*)((uintptr_t)sp & ~0xF); 
        *(--sp) = 0; 
        *(--sp) = (uintptr_t)scheduler_loop;
        sp -= 6;
        scheduler_rsp = (void *)sp;

        const char *clk = getenv("FIBER_CLOCK");
        if (clk && strcmp(clk, "real") == 0) {
            clock_mode_real = 1;
            init_crit_bounds();
            
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_sigaction = timer_handler;
            sa.sa_flags = SA_RESTART | SA_SIGINFO; 
            sigaction(SIGALRM, &sa, NULL);

            struct sigevent sev;
            memset(&sev, 0, sizeof(sev));
            sev.sigev_notify = SIGEV_SIGNAL;
            sev.sigev_signo = SIGALRM;
            
            timer_create(CLOCK_PROCESS_CPUTIME_ID, &sev, &global_timerid);
            
            const char *q_env = getenv("FIBER_QUANTUM_US");
            long quantum_us = q_env ? atol(q_env) : 1000;
            struct itimerspec its;
            its.it_interval.tv_sec = quantum_us / 1000000;
            its.it_interval.tv_nsec = (quantum_us % 1000000) * 1000;
            its.it_value = its.it_interval;
            timer_settime(global_timerid, 0, &its, NULL);
        }

        current = calloc(1, sizeof(fiber_node_t));
        current->stat.id = FIBER_MAIN;
        current->stat.state = FIBER_RUNNABLE;
        current->stat.stack_mode = STACK_OWN;
        current->slice_remaining = 64; 
        current->block_type = BLOCK_NONE;
        current->io_fd = -1;
        current->fpu_cw = default_fpu_cw;
        current->mxcsr = default_mxcsr;
        register_fiber(current);
        add_runnable(current);
    }
}

void fiber_yield(void) {
    API_ENTER(os);
    int saved_err = errno; 

    if (current->stat.state != FIBER_RUNNABLE) {
        remove_runnable(current);
    }

    if (epoll_fd >= 0 && io_waitcount > 0) {
        int timeout = (run_list == NULL) ? -1 : 0; 
        struct epoll_event events[8];
        int n_events;
        do {
            n_events = epoll_wait(epoll_fd, events, 8, timeout);
        } while (n_events < 0 && errno == EINTR && timeout == -1); 
        
        if (n_events > 0) {
            for (int i = 0; i < n_events; i++) {
                int ready_fd = events[i].data.fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ready_fd, NULL);
                if (ready_fd >= 0 && ready_fd < MAX_FDS) {
                    fiber_node_t *w = fd_waiters[ready_fd];
                    if (w) {
                        fd_waiters[ready_fd] = w->next_waiter;
                        w->io_fd = -1;
                        io_waitcount--;
                        add_runnable(w);
                        
                        if (fd_waiters[ready_fd]) {
                            struct epoll_event ev;
                            memset(&ev, 0, sizeof(ev));
                            ev.events = EPOLLIN | EPOLLONESHOT;
                            ev.data.fd = ready_fd;
                            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ready_fd, &ev);
                        }
                    }
                }
            }
        }
    }

    if (!run_list) {
        fprintf(stderr, "contextchaos: deadlock\n");
        abort();
    }

    fiber_node_t *prev = current;
    fiber_node_t *next = run_list;
    if (prev->is_in_runqueue) {
        next = prev->run_next;
    } else {
        fiber_node_t *tmp = run_list;
        do {
            if (tmp->stat.id > prev->stat.id) {
                next = tmp;
                break;
            }
            tmp = tmp->run_next;
        } while (tmp != run_list);
    }
    
    if (prev == next) {
        int q = 64 / (current->stat.priority + 1);
        if ((64 % (current->stat.priority + 1)) != 0) q++;
        current->slice_remaining = q;

        errno = saved_err;
        API_LEAVE(os);
        return;
    }

    current = next;
    int q = 64 / (current->stat.priority + 1);
    if ((64 % (current->stat.priority + 1)) != 0) q++;
    current->slice_remaining = q;
    
    if (prev->is_in_runqueue && prev->slice_remaining > 0) prev->stat.yields++;
    current->stat.switches_in++;
    
    sched_prev = prev;
    sched_next = current;
    
    prev->saved_errno = saved_err;
    
    __asm__ volatile("fnstcw %0" : "=m"(prev->fpu_cw));
    __asm__ volatile("stmxcsr %0" : "=m"(prev->mxcsr));

    if (prev->stat.stack_mode == STACK_OWN && current->stat.stack_mode == STACK_OWN) {
        fiber_switch(&prev->rsp, current->rsp);
    } else {
        fiber_switch(&prev->rsp, scheduler_rsp);
    }
    
    __asm__ volatile("fldcw %0" : : "m"(current->fpu_cw));
    __asm__ volatile("ldmxcsr %0" : : "m"(current->mxcsr));

    errno = current->saved_errno;
    
    API_LEAVE(os);
    
    if (current->pending_preemption && current->preempt_disable_count == 0) {
        current->pending_preemption = 0;
        current->stat.preemptions++;
        fiber_yield();
    }
}

static void wrapper(void) {
    __asm__ volatile("fldcw %0" : : "m"(current->fpu_cw));
    __asm__ volatile("ldmxcsr %0" : : "m"(current->mxcsr));
    
    errno = current->saved_errno;
    
    if (clock_mode_real) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        sigprocmask(SIG_UNBLOCK, &set, NULL);
    }
    
    if (current->pending_preemption && current->preempt_disable_count == 0) {
        current->pending_preemption = 0;
        current->stat.preemptions++;
        fiber_yield();
    }
    
    current->fn(current->arg);
    fiber_exit(NULL);
}

fiber_t fiber_spawn(void (*fn)(void *), void *arg, const fiber_attr_t *attr) {
    ensure_init();
    API_ENTER(os);

    if (attr && attr->priority > 31) {
        errno = EINVAL;
        API_LEAVE(os);
        return FIBER_INVALID;
    }

    fiber_node_t *node = calloc(1, sizeof(fiber_node_t));
    node->stat.id = next_id++;
    node->stat.state = FIBER_RUNNABLE;
    node->stat.priority = attr ? attr->priority : 0;
    node->stat.stack_mode = attr ? attr->stack_mode : STACK_OWN;
    node->fn = fn;
    node->arg = arg;
    node->block_type = BLOCK_NONE;
    node->io_fd = -1;
    node->fpu_cw = default_fpu_cw;
    node->mxcsr = default_mxcsr;
    register_fiber(node);
    add_runnable(node);
    
    uintptr_t *sp;
    if (node->stat.stack_mode == STACK_OWN) {
        size_t actual_size = (attr && attr->stack_size > 0) ? attr->stack_size : 65536;
        node->stack_size = actual_size;
        node->stack_base = mmap(NULL, actual_size + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        mprotect(node->stack_base, 4096, PROT_NONE);
        sp = (uintptr_t *)((char *)node->stack_base + 4096 + actual_size);
    } else {
        sp = (uintptr_t *)shared_stack_top;
    }
    
    sp = (uintptr_t *)((uintptr_t)sp & ~0xF); 
    *(--sp) = 0; 
    *(--sp) = (uintptr_t)wrapper;
    sp -= 6;
    
    if (node->stat.stack_mode == STACK_SHARED) {
        size_t used = (char*)shared_stack_top - (char*)sp;
        node->stat.saved_stack_bytes = used;
        node->stat.max_saved_stack_bytes = used;
        node->saved_stack = malloc(used);
        memcpy(node->saved_stack, sp, used);
    }
    
    node->rsp = sp;
    API_LEAVE(os);
    return node->stat.id;
}

void fiber_exit(void *ret) {
    API_ENTER(os);
    current->stat.state = FIBER_DONE;
    current->retval = ret;
    
    if (current->joiner) {
        current->joiner->block_type = BLOCK_NONE;
        current->joiner->block_target = NULL;
        add_runnable(current->joiner);
        current->joiner = NULL;
    }
    fiber_yield();
    __builtin_unreachable();
}

fiber_t fiber_self(void) {
    ensure_init();
    return current->stat.id;
}

int fiber_join(fiber_t f, void **ret) {
    ensure_init();
    API_ENTER(os);
    
    if (f < 0 || f >= (fiber_t)fibers_cap || !fibers[f]) {
        API_LEAVE(os);
        return ESRCH;
    }
    fiber_node_t *target = fibers[f];
    
    if (f == current->stat.id) {
        API_LEAVE(os);
        return EDEADLK; 
    }
    if (target->joiner != NULL) {
        API_LEAVE(os);
        return EINVAL; 
    }
    
    fiber_node_t *trace = target;
    while (trace) {
        if (trace == current) {
            API_LEAVE(os);
            return EDEADLK;
        }
        if (trace->block_type == BLOCK_MUTEX) {
            mutex_internal_t *m = (mutex_internal_t *)((fmutex_t *)trace->block_target)->opaque;
            trace = m->owner;
        } else if (trace->block_type == BLOCK_JOIN) {
            trace = (fiber_node_t *)trace->block_target;
        } else {
            break;
        }
    }
    
    if (target->stat.state != FIBER_DONE) {
        current->block_type = BLOCK_JOIN;
        current->block_target = target;
        target->joiner = current;
        current->stat.state = FIBER_BLOCKED;
        
        fiber_yield(); 
        
        current->block_type = BLOCK_NONE;
        current->block_target = NULL;
    }
    
    if (ret) *ret = target->retval;
    
    if (target->stat.stack_mode == STACK_OWN) {
        munmap(target->stack_base, target->stack_size + 4096);
    } else {
        free(target->saved_stack);
    }
    free(target);
    fibers[f] = NULL; 
    
    API_LEAVE(os);
    return 0;
}

int fiber_stats(fiber_t f, fiber_stat_t *out) {
    ensure_init();
    API_ENTER(os);
    
    if (f < 0 || f >= (fiber_t)fibers_cap || !fibers[f]) {
        API_LEAVE(os);
        return ESRCH;
    }
    if (out) *out = fibers[f]->stat;
    
    API_LEAVE(os);
    return 0;
}

void fiber_tick(void) {
    ensure_init();
    if (clock_mode_real) return;
    
    API_ENTER(os);
    current->stat.ticks++;
    current->slice_remaining--;
    
    if (current->slice_remaining <= 0) {
        if (current->preempt_disable_count > 0) {
            current->pending_preemption = 1;
        } else {
            current->stat.preemptions++; 
            fiber_yield();
        }
    }
    API_LEAVE(os);
}

int fmutex_init(fmutex_t *m) {
    if (!m) return EINVAL;
    API_ENTER(os);
    mutex_internal_t *mi = (mutex_internal_t *)m->opaque;
    mi->owner = NULL; 
    mi->waiters_head = NULL; 
    mi->acquired_at_tick = 0;
    API_LEAVE(os);
    return 0;
}

int fmutex_trylock(fmutex_t *m) {
    ensure_init();
    API_ENTER(os);
    mutex_internal_t *mi = (mutex_internal_t *)m->opaque;
    if (mi->owner != NULL) {
        API_LEAVE(os);
        return EBUSY;
    }
    
    mi->owner = current;
    mi->acquired_at_tick = current->stat.ticks;
    API_LEAVE(os);
    return 0;
}

int fmutex_lock(fmutex_t *m) {
    ensure_init();
    API_ENTER(os);
    mutex_internal_t *mi = (mutex_internal_t *)m->opaque;
    
    if (mi->owner == current) {
        API_LEAVE(os);
        return EDEADLK;
    }
    
    fiber_node_t *trace = mi->owner;
    while (trace) {
        if (trace == current) {
            API_LEAVE(os);
            return EDEADLK;
        }
        if (trace->block_type == BLOCK_MUTEX) {
            mutex_internal_t *next_m = (mutex_internal_t *)((fmutex_t *)trace->block_target)->opaque;
            trace = next_m->owner;
        } else if (trace->block_type == BLOCK_JOIN) {
            trace = (fiber_node_t *)trace->block_target;
        } else {
            break;
        }
    }
    
    if (mi->owner == NULL) {
        mi->owner = current;
        mi->acquired_at_tick = current->stat.ticks;
        API_LEAVE(os);
        return 0;
    }
    
    current->block_type = BLOCK_MUTEX;
    current->block_target = m;
    current->stat.state = FIBER_BLOCKED;
    
    current->next_waiter = NULL;
    if (mi->waiters_head == NULL) {
        mi->waiters_head = current;
    } else {
        fiber_node_t *w = mi->waiters_head;
        while (w->next_waiter) w = w->next_waiter;
        w->next_waiter = current;
    }
    
    fiber_yield(); 
    
    current->block_type = BLOCK_NONE;
    current->block_target = NULL;
    API_LEAVE(os);
    return 0;
}

int fmutex_unlock(fmutex_t *m) {
    ensure_init();
    API_ENTER(os);
    mutex_internal_t *mi = (mutex_internal_t *)m->opaque;
    
    if (mi->owner != current) {
        API_LEAVE(os);
        return EPERM;
    }
    
    current->stat.lock_hold_ticks += (current->stat.ticks - mi->acquired_at_tick);
    
    if (mi->waiters_head == NULL) {
        mi->owner = NULL; 
        API_LEAVE(os);
        return 0;
    }
    
    fiber_node_t *best_prev = NULL;
    fiber_node_t *best = mi->waiters_head;
    fiber_node_t *prev = mi->waiters_head;
    fiber_node_t *curr = prev->next_waiter;
    
    while (curr) {
        if (curr->stat.lock_hold_ticks > best->stat.lock_hold_ticks ||
           (curr->stat.lock_hold_ticks == best->stat.lock_hold_ticks && curr->stat.id < best->stat.id)) {
            best = curr; 
            best_prev = prev;
        }
        prev = curr; 
        curr = curr->next_waiter;
    }
    
    if (best_prev == NULL) mi->waiters_head = best->next_waiter;
    else best_prev->next_waiter = best->next_waiter;
    
    mi->owner = best;
    best->next_waiter = NULL;
    mi->acquired_at_tick = best->stat.ticks;
    
    best->block_type = BLOCK_NONE;
    best->block_target = NULL;
    add_runnable(best);
    
    API_LEAVE(os);
    return 0;
}

int fmutex_destroy(fmutex_t *m) {
    if (!m) return EINVAL;
    API_ENTER(os);
    mutex_internal_t *mi = (mutex_internal_t *)m->opaque;
    if (mi->owner != NULL || mi->waiters_head != NULL) {
        API_LEAVE(os);
        return EBUSY;
    }
    API_LEAVE(os);
    return 0;
}

void fiber_preempt_disable(void) { 
    ensure_init();
    API_ENTER(os);
    current->preempt_disable_count++; 
    API_LEAVE(os);
}

void fiber_preempt_enable(void) {
    ensure_init();
    API_ENTER(os);
    current->preempt_disable_count--;
    if (current->preempt_disable_count == 0 && current->pending_preemption) {
        current->pending_preemption = 0;
        current->stat.preemptions++;
        fiber_yield();
    }
    API_LEAVE(os);
}

ssize_t fiber_read(int fd, void *buf, size_t n) {
    ensure_init();
    API_ENTER(os);
    
    if (fd >= 0 && fd < MAX_FDS) {
        current->next_waiter = fd_waiters[fd];
        fd_waiters[fd] = current;
    }
    
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = fd; 
    
    int res = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    if (res < 0 && errno == EEXIST) {
        res = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
    
    if (res >= 0) {
        io_waitcount++;
        current->io_fd = fd;
        current->stat.state = FIBER_BLOCKED;
        fiber_yield(); 
    } else {
        if (fd >= 0 && fd < MAX_FDS) {
            if (fd_waiters[fd] == current) {
                fd_waiters[fd] = current->next_waiter;
            } else {
                fiber_node_t *w = fd_waiters[fd];
                while (w && w->next_waiter != current) w = w->next_waiter;
                if (w) w->next_waiter = current->next_waiter;
            }
        }
    }
    
    API_LEAVE(os);
    return read(fd, buf, n);
}