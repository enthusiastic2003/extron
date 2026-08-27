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

## Resource limits and accounting

Resource limits are process-wide, shared by every thread, inherited by
`fork()`, and preserved by `execve()`. `RLIMIT_NOFILE` is enforced by the
common descriptor allocator for open, pipe, dup, and fcntl duplication;
lowering it does not invalidate descriptors that are already open.
`sysconf(_SC_OPEN_MAX)` reports that process's current soft limit. The fixed
128 KiB userspace stack and the absence of core dumps are reported truthfully
as fixed `RLIMIT_STACK` and zero `RLIMIT_CORE` values. Other limits remain
unlimited, and attempts to install a finite value are rejected until the
kernel has the machinery to enforce it.

High-resolution user and system CPU time is charged at exception boundaries
and context switches. Sleeping, stopped, and idle time is excluded; worker
threads contribute to the owning process. Reaping transfers a child's own and
already-reaped descendant usage into the parent's `RUSAGE_CHILDREN` totals,
and the resource-aware wait path supplies per-child usage to `wait4()`.
Resource fields for which the kernel has no accounting yet remain zero.

## Signals

The signal-delivery layer supports process-wide dispositions and pending
state, thread-directed pending state, per-thread masks, `signal()`/
`sigaction()`, `raise()`/PID and process-group `kill()`, `sigprocmask()`/
`pthread_sigmask()`, and AArch64 signal frames. `SA_SIGINFO` handlers receive
real `siginfo_t` and `ucontext_t` objects containing the sender or fault
details and the interrupted machine context.
Handler return enters an mlibc restorer which calls `SYS_SIGRETURN`; the kernel
then restores all general registers, FP/SIMD registers, FPCR/FPSR, TPIDR_EL0,
the interrupted stack/PC, and the previous mask. Synchronous EL0 faults enter
an installed handler or retain their default process-termination behavior.

`SYS_TGKILL` targets a signal at one TID in a process. mlibc uses it for
deferred `pthread_cancel()`: the cancellation signal interrupts supported
blocking cancellation points (`read`, blocking pipe `write`, `sleep`, `poll`,
and futex wait), the handler verifies `SI_TKILL` and the interrupted PC, runs
pthread cleanup handlers, and exits only the selected thread. A following
`pthread_join()` returns `PTHREAD_CANCELED`. Disabled cancellation remains
queued until re-enabled and a cancellation point is reached.

Dispositions are inherited by `fork()`. `execve()` resets caught handlers to
default, preserves ignored dispositions, clears pending signals, and preserves
the calling thread's mask.

The default actions for `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, and `SIGTTOU` stop
all threads in a process; `SIGCONT` resumes them, and `SIGKILL` can terminate a
stopped process. Processes carry session and process-group IDs. `setpgid()`,
`getpgid()`, `setsid()`, foreground-TTY `TIOCGPGRP`/`TIOCSPGRP`, and the
corresponding mlibc interfaces let BusyBox ash create and control jobs.
Terminal Ctrl-C, Ctrl-\\, and Ctrl-Z are recognized in the interrupt-driven
UART receive path and sent to the foreground group even when its program is
not reading stdin.

Stopping preserves each thread's prior runnable or sleeping state together
with its wait channel and timeout. Wakeups and deadline expiry that happen
while a process is stopped are recorded without scheduling it; `SIGCONT`
then either restores the original blocked wait or makes the thread runnable
if its event already occurred. A stopped pipe read therefore still receives
its data, and a stopped `sleep()` retains its original deadline.

Parents receive `SIGCHLD` with child identity and exit, signal, stop, or
continue information. `waitpid()` implements exact-PID and process-group
selection plus `WNOHANG`, `WUNTRACED`, and `WCONTINUED`; stop and continue
events do not reap the child. `SA_NOCLDSTOP` suppresses those two `SIGCHLD`
notifications.

PID reservation is separate from process-table publication. A process is
fully initialized—including its thread list and kernel stack—before timer or
wakeup code can discover it in the global table.

## Known limits and next steps

- Futexes are private to one process and use a fixed 256-channel table. Shared
  memory futexes will require physical-page-based keys.
- The mlibc-created userspace stack has no guard page because Extron does not
  yet implement anonymous `mmap`; it uses the existing anonymous allocator.
- Robust mutexes, scheduling attributes, and SMP locking guarantees are not
  implemented. Deferred pthread cancellation is validated; asynchronous
  cancellation remains unvalidated and is unsafe around userspace locks.
- Alternate signal stacks, real-time queues, `kill(-1)`, public
  `pthread_kill()`, `sigsuspend()`, `sigtimedwait()`, `SA_NOCLDWAIT`, and
  `SA_RESTART` are not implemented. Supported blocking calls return `EINTR`;
  interruption is not yet generalized to every future blocking syscall.
- Job control currently has one global console TTY. It does not yet model
  controlling-terminal acquisition, orphaned process groups, session-leader
  hangup, or background TTY read/write enforcement.
- Add an mlibc-supported stack unmap/reap path once the libc side resolves its
  current join cleanup FIXME.

Pending process signals belong to `struct proc`, while masks and delivery
state belong to `struct thread`. SMP remains a later concern; useful blocking
concurrency does not require it.
