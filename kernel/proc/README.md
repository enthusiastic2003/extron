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

Every process embeds its main thread (TID equals PID) and may own dynamically
allocated worker threads with independent TIDs and guarded kernel stacks.
Workers share the process address space, file table, cwd, and parentage while
retaining separate CPU/FP/TLS state and user stacks.

Syscalls 29--34 provide `gettid`, thread create, thread exit, thread join, and
private futex wait/wake. Thread creation accepts an entry trampoline, initial
userspace SP, AArch64 TLS pointer, and userspace completion word. Thread exit
sets that word to one with release ordering and wakes its futex waiters. Raw
join waits for `THREAD_EXITED` and reclaims the dynamic kernel thread object.

Extron's mlibc sysdeps connect those primitives to ordinary `pthread_create`,
`pthread_join`, mutexes, condition variables, and per-thread TLS. No generic
mlibc source is patched. mlibc currently has an upstream FIXME that leaves the
TCB and userspace stack allocated after POSIX join; the kernel objects remain
until process teardown unless userspace also invokes Extron's raw join/reap.

`fork()` creates a new process containing only the calling thread and resumes
from its trap frame. A successful `execve()` terminates all sibling threads,
removes stale run-queue and futex membership, and then resumes only the caller
in the replacement image. Process exit terminates every thread, closes shared
descriptors once, and leaves a process zombie; `wait()` destroys all remaining
thread stacks and the shared address space from the parent's context.

An unhandled synchronous exception from EL0 follows that same process-exit
path. It terminates the faulting process and all of its threads, then wakes only
the direct parent. The raw kernel status is a negative signal number; Extron's
mlibc translates it into POSIX wait status so `WIFSIGNALED()` and `WTERMSIG()`
work. An EL1 exception remains a kernel panic.

PID reservation is separate from process-table publication. A process is
fully initialized—including its thread list and kernel stack—before timer or
wakeup code can discover it in the global table.

## Known limits and next steps

- Futexes are private to one process and use a fixed 256-channel table. Shared
  memory futexes will require physical-page-based keys.
- The mlibc-created userspace stack has no guard page because Extron does not
  yet implement anonymous `mmap`; it uses the existing anonymous allocator.
- There are no cancellation signals, robust mutexes, scheduling attributes,
  or SMP locking guarantees yet.
- Add process-wide signal state and per-thread masks/delivery state before
  implementing pthread cancellation.
- Add an mlibc-supported stack unmap/reap path once the libc side resolves its
  current join cleanup FIXME.

Signals should build on this model: pending process signals belong to
`struct proc`, while masks and delivery state belong to `struct thread`.
SMP remains a later concern; useful blocking concurrency does not require it.
