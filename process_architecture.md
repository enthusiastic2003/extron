# Extron OS: Process Architecture & Scheduling

This document details the process lifecycle, scheduling mechanisms, and user/kernel space transitions in the Extron kernel. It serves as an architectural reference for current behaviors and future developments like `fork()` and multithreading.

## 1. Process Scheduling & Context Switching

The kernel implements a **non-preemptive (cooperative) kernel** with a **preemptive userspace**. This means a process running in kernel mode (e.g., inside a syscall) cannot be preempted by a timer interrupt; scheduling only occurs if the interrupt originated from Ring 3 (userspace) or if the kernel voluntarily yields/sleeps.

### The Scheduler (`schedule()`)
The scheduler is invoked via timer interrupts (from Ring 3) or blocking syscalls (e.g., `sleep`). It performs the following:
1. **Selection**: Dequeues the next `PROC_RUNNABLE` process from the `run_queue`.
2. **Installation (`proc_install`)**: 
   - **TSS `rsp0`**: Sets the hardware Task State Segment `rsp0` to point to the new process's `kernel_stack_top`. This guarantees that the next Ring 3 -> Ring 0 hardware interrupt will start at a perfectly clean kernel stack.
   - **CR3**: Switches the page directory (`load_cr3`) to the new process's address space.
   - **TLS / FS Base**: Restores the `IA32_FS_BASE` MSR for Thread-Local Storage using `write_fs_base()`.
3. **Context Switch (`context_switch`)**: An assembly routine that saves the callee-saved registers (R12-R15, RBX, RBP) and the current `RIP`/`RSP` into the outgoing process's `struct cpu_context`, and loads them from the incoming process's `context`.

## 2. The Init Process

The `init` process is the very first process launched by the kernel (`kernel_stage2`).

### Bootstrapping `init`
1. **Creation**: `create_init_proc()` allocates a new page table, loads the ELF binary into it, and allocates a user stack.
2. **Fake Trap Frame**: A `struct trap_frame` is artificially constructed at the top of the process's kernel stack. This frame is pre-populated with the target `RIP` (ELF entry point), `RSP` (user stack), `USER_CS`, and `USER_DS`.
3. **Dispatch (`sched_start`)**: 
   - Dequeues `init`.
   - Calls `proc_install()` to set the TSS and load CR3.
   - Caches the `RIP` and `RSP` from the trap frame.
   - **Crucial Reset**: Resets `p->kernel_rsp = p->kernel_stack_top`. The trap frame was only needed to hold the initial values to get into Ring 3. It is discarded to provide a clean stack for future fast syscalls.
   - Calls `enter_userspace()`, executing an `iretq` or `sysret` to drop the CPU into Ring 3.

*(Note: Direct binary loading for subsequent processes via `load_executable_from_binary` was a temporary test measure and is deprecated in favor of future standard process creation via `fork`/`exec`)*.

## 3. Privilege Transitions (Ring 3 <-> Ring 0)

Transitions between userspace and kernelspace occur via two distinct mechanisms: Hardware Interrupts and Fast System Calls.

### Hardware Interrupts (Timer, Keyboard, etc.)
1. **Entry**: The CPU detects an interrupt while in Ring 3. Because of the privilege change, it reads the TSS, loads `rsp = TSS.rsp0` (which is `kernel_stack_top`), and pushes the hardware interrupt frame: `SS, RSP, RFLAGS, CS, RIP`.
2. **Handler**: The ISR assembly pushes the remaining registers, forming a complete `trap_frame`, and calls the C interrupt handler.
3. **Exit**: The C handler returns, and the ISR pops the registers, finally executing `iretq` to return to Ring 3.

### Fast System Calls (`SYSCALL` instruction)
1. **Entry (`syscall_entry.asm`)**: The `SYSCALL` instruction does **not** use the TSS. Instead, it jumps directly to the kernel's LSTAR MSR handler.
2. **Stack Switch**: The assembly handler manually reads `current_proc` and forcibly sets `rsp = current_proc->kernel_stack_top`.
3. **State Save**: It pushes the caller/callee saved registers and the special syscall return state (`rcx` for RIP, `r11` for RFLAGS).
4. **Dispatch**: Calls the C `syscall_dispatch`.
5. **Exit**: Pops the state and executes `iretq` to return to Ring 3.

## 4. The Process Lifecycle

1. **PROC_UNUSED**: Process structure allocated, but not fully initialized.
2. **PROC_RUNNABLE**: Ready to run. Memory mapped, trap frame or context prepared. Resides in the `run_queue`.
3. **PROC_RUNNING**: Currently executing on the CPU (`current_proc`).
4. **PROC_SLEEPING**: Blocked on an event (e.g., timer, I/O). Removed from `run_queue`, waiting in the global process table.
5. **PROC_ZOMBIE**: Terminated via `sys_exit`. Awaiting cleanup by parent (to be handled by a future `wait()` syscall).

## 5. Roadmap: Preparing for `fork()` and Threads

To successfully implement `fork()` and multithreading, the following prerequisites must be built:

### Memory Management (For `fork`)
- **`vm_space_clone`**: A function capable of duplicating an entire userspace address space. It must allocate a new PML4, iterate the parent's mappings, allocate physical frames for the child, and `memcpy` the data. 
- **Future Optimization**: Implementing Copy-on-Write (CoW) by marking pages Read-Only and handling Page Faults.

### Architecture State (For `fork`)
- **Syscall Frame Exposure**: Currently, `syscall_dispatch` receives only `arg1, arg2, arg3`. `syscall_entry.asm` must be modified to pass a pointer to the pushed register state (either a `syscall_frame` or `trap_frame`) into `syscall_dispatch`.
- **Child Trap Frame Creation**: `sys_fork` will need this exposed frame to manually duplicate the parent's exact CPU state onto the child's clean kernel stack. It must specifically override `RAX = 0` in the child's frame so the child process knows it is the child.
- **Context Resumption**: The child's `context.rip` will be set to a new function (e.g. `fork_ret`) which simply returns from the context switch, restores the duplicated trap frame, and jumps to userspace.

### Threading Prerequisites
- **Shared `vm_space`**: Threads are essentially processes that share the same `mm` pointer and CR3.
- **Unique Stacks**: The kernel will need to allocate distinct user stacks within that shared `vm_space` for each thread.
- **TLS Management**: Ensure `sys_tcb_set` remains strictly per-thread (updating `p->fs_base`).
