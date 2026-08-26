# Scheduler

## Shape

UP, 32 priority levels (0 idle … 31 realtime), round-robin within a
level, 100 Hz quantum. Preemptive. Not the Linux CFS. Not a real-time
OS. A Windows engineer should see "dispatcher database" and nod.

## Why PIT, not LAPIC, in v1

PIT channel 0 + PIC IRQ0 is present on every QEMU/q35 we care about
and does not require MADT, MSR `APIC_BASE`, or an x2APIC enable
sequence. We already have to remap the PIC for exceptions-vs-IRQs.
The 100 Hz tick is the clock interrupt; `ke_tick` is the one function
that may set `need_resched`.

LAPIC timer replaces PIT in the HAL without touching `ke/sched.c`.
Contract: `hal_timer_init(hz)` and `hal_timer_isr` call `ke_on_tick()`.

## Thread states

```
UNUSED → READY ⇄ RUNNING → WAITING
                 RUNNING → TERMINATED
                 WAITING → READY
```

`TERMINATED` stays referenced until the last handle and the reaper
drop it. The thread object is waitable and is signaled on the way to
`TERMINATED`.

## Quantum

Default 3 ticks (30 ms at 100 Hz). Realtime (priority ≥ 24) is still
preempted by higher realtime, but does not rotate on quantum end —
it yields only on wait or `NtYieldExecution`. That is how a runaway
realtime thread burns the box; it is also how a driver DPC-equivalent
(we don't have DPCs as threads) would. v1 has no watchdog.

Idle thread: priority 0, always ready, runs `hlt` with IF set.

## Ready queues

`ready[32]` doubly-linked. `ready_mask` is a 32-bit bitset so
`highest = 31 - clz(ready_mask)`. Insertion: tail (RR). Preempt if
the incoming priority is **strictly greater** than current.

No priority inheritance in v1 except mutex wait-boost: waiter donates
its priority to the mutex owner until release. Boost is stored on the
thread (`saved_priority`) and unwound on `NtReleaseMutex`. Nested
mutexes: one saved priority, the highest donation sticks. This is
incomplete and documented; it is not a spinlock pretending to be a
mutex.

## Context switch (`ke/switch.S`)

Callee-saved + `rip`/`rsp`/`rflags`:

```
r15 r14 r13 r12 rbx rbp
rip rsp rflags
```

FXSAVE is **not** in v1. User threads that execute SSE will leak/clobber
XMM. Residual, tracked: we zero XMM on thread create so at least we
do not leak kernel SIMD. Preempted user SSE is wrong. Fix before any
libc that uses SSE memcpy. `clang -mno-sse` for kernel; user is on
its own until `fxsave` lands.

Switch runs with IF off. The incoming thread restores `rflags` from
its frame, which may enable IF.

## Wait

See [OBJECT_MODEL.md](OBJECT_MODEL.md). `KeWaitForSingleObject` is the
only sleep. `KeDelayExecution` waits on a per-thread timer. There is
no `sleep()` in the kernel that busy-waits more than 1 µs (port I/O
spin on UART LSR is the exception, and it is capped).

Timeouts are in 100 ns units (NT) but our tick is 10 ms, so the
minimum real sleep is one tick. A 0 timeout is a poll.

## Idle / start

`sched_start` switches from the boot context into idle. Boot context
is never returned to. Idle then picks init if it is ready (it is).

## Failure modes

| Mode | Result |
|---|---|
| No ready thread except idle | idle `hlt` |
| Stack smash | DF on IST1, panic |
| Wait with locks held | lock-rank assert in debug; in release we still wait and that is a deadlock — debug builds `panic` if IRQL > PASSIVE on wait |
| Priority inversion (non-mutex) | accepted in v1 |
| init (pid 1) exits | panic `"init died"` |

## Lock ranking

```
9   DISP    (dispatcher object)
10  SCHED   (ready queues)
```

You may take a wait-object lock then the scheduler lock. You may not
take the scheduler lock then a wait-object lock. T3 reversal: v0.3 had
this backwards and ping/pong paniced. `sched_ready` is idempotent
(READY/RUNNING is a no-op) so WaitForMultiple cannot double-insert.

On last thread of a process, `sched_exit_thread` drops PROC **before**
destroying the aspace and handle table (VMM=4, HANDLE=7 are below PROC=8).

Hardware switch loads CR3 from `next->process->aspace.cr3_phys` and
`tss_set_rsp0` to the top of the new kstack.

## Reversal log

none. 4-class MLFQ was considered; 32-level RR is what we will
measure against in the performance pass.

T6: `NtWaitForMultipleObjects` enqueues a wait_block on every object,
wakes on any (WAIT_ANY) or retries until all signaled (WAIT_ALL).
Timeout scan in `ke_on_tick` unlinks every wait_block, not just `t->wait`.
Kstack canary checked on exit.

T8: hardware kstack is `KERNEL_STACK_BASE + (tid-1)*KSTACK_STRIDE`,
page 0 not-present, 16 KiB RW+NX, then a hole. Overflow is a kernel
#PF (panic), not a heap smash. Host still uses posix_memalign + `0xA5`
canary. Lazy FPU: `fpu_lazy_switch` sets `CR0.TS`; `#NM` FXSAVE/FXRSTOR;
kernel `#NM` panics. Thread exit calls `fpu_drop` then unmaps the
kstack. WAIT_ALL is covered by host selftest (notification events).

T9: `tss_map_ist` after `vmm_init` points IST1–4 at guarded stacks at
`IST_STACK_BASE`. DF/NMI/MC/IRQ no longer land in `.bss`.

T12: mutex wait-boost is real. `sched_boost` requeues a READY owner
under SCHED (legal while holding DISP). Nested mutexes: one
`saved_priority`, unwind only when `owned_mutexes` is empty. Thread
death abandons every owned mutex before signaling the thread object.
`NtTerminateThread` on another thread sets `kill_pending`; trampoline
and switch-in call `sched_exit_thread`. Host: `sched_start` zeros
idle's ucontext `valid` bit; last non-idle exit `longjmp`s from the
dying kstack so we never `setcontext` a post-longjmp idle frame.

T14: WAIT_ALL fast path uses `disp_satisfied`: a mutex the caller
already owns counts, even though `signal_state` is 0 while held.
A timeout-0 WAIT_ALL on (owned mutex, signaled event) is SUCCESS,
not TIMEOUT. Consume still goes through `ke_wait_object` and bumps
mutex recursion — two `NtReleaseMutex` to fully drop.

T15: last-thread exit still `vmm_aspace_destroy` then `ht_destroy`.
Destroy snapshots VADs, unmaps rank-safe (no PMM under VMM), then
frees user page tables. `user_launch` (hardware) writes argc/argv[0]
before `enter_user`; extra operands are T16.

T16: `builtin_entry` and `elf_host_stub` consume `p->argc`/`p->argv`.
Hardware `user_launch` still `enter_user(entry, user_stack)` — the
stack pointer is the argc slot. No scheduler change.

T17: `psp_create_process` fails closed if `ob_create(OBJ_TOKEN)`
returns NULL (destroys aspace first). System process also attaches
a token. `sched_ready` is unchanged (already idempotent on
READY/RUNNING).







T19: `psp_create_process` snapshots `parent->token->integrity` onto the
new token. No ready-queue change.

