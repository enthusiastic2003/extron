# Processes and threads

Extron distinguishes resource ownership from scheduling:

- `struct proc` owns the address space, descriptor table, cwd, process ID,
  parent/child relationship, program arguments, and process exit status.
- `struct thread` owns the CPU context (including FP/SIMD and `TPIDR_EL0`),
  guarded kernel stack, initial user entry/stack, scheduler state, wait
  channel, and timed-sleep deadline.

The round-robin run queue contains threads. `my_thread()` returns the current
schedulable object; `my_proc()` derives its owning process. A context switch
changes TTBR0 only when the incoming thread belongs to a different address
space, so future threads in one process will share mappings without redundant
TLB flushes.

## Current milestone

Every process currently embeds one main thread. Its TID equals the PID, and
the process keeps a thread list so wakeups, timed sleeps, diagnostics, and
teardown already operate on thread objects. There is deliberately no
userspace thread-creation ABI yet.

`fork()` creates a new process with one thread that resumes from the caller's
trap frame. `execve()` replaces the current process image and updates the
calling thread's entry/stack bookkeeping. Process exit closes process-owned
descriptors and marks its sole thread exited; `wait()` later destroys every
thread stack and the shared address space from the parent's context.

PID reservation is separate from process-table publication. A process is
fully initialized—including its thread list and kernel stack—before timer or
wakeup code can discover it in the global table.

## Next threading ABI

The next incremental steps are:

1. allocate independent TIDs and dynamically allocated thread objects;
2. add a clone-style syscall that shares the process address space and files,
   accepts a user stack, and installs a distinct TLS pointer;
3. separate thread exit from final process exit and implement join/clear-TID;
4. implement futex wait/wake for libc synchronization;
5. teach `execve()` to terminate sibling threads and preserve POSIX fork's
   calling-thread-only behavior;
6. connect the Extron mlibc sysdeps to mlibc's pthread implementation.

Signals should build on this model: pending process signals belong to
`struct proc`, while masks and delivery state belong to `struct thread`.
SMP remains a later concern; useful blocking concurrency does not require it.
