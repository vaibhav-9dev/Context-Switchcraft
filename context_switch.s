.globl fiber_switch
.type fiber_switch, @function
fiber_switch:
    # rdi = old_rsp ptr, rsi = new_rsp
    # Save callee-saved registers
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    # Save current rsp
    mov %rsp, (%rdi)
    # Switch to new rsp
    mov %rsi, %rsp
    # Restore callee-saved
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
.size fiber_switch, .-fiber_switch
