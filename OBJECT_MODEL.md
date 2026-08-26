# Object model

NT-shaped. Userland never holds a kernel pointer. Everything interesting
is an object; everything a process can name is a handle.

## Header

```
object_header
  type            pointer to object_type (not a magic int)
  name            counted UTF-8, optional, lives in the directory
  directory       parent Directory, or NULL if unnamed
  pointer_count   kernel references (atomic u64)
  handle_count    open handles across all processes (atomic u64)
  flags           PERMANENT | KERNEL_ONLY | DELETE_PENDING | INHERIT
  sd              security descriptor stub (owner pid + world mask v1)
  wait_hdr        dispatcher header if waitable, else NULL
  body            type-specific, immediately follows the header
```

Invariant: `pointer_count >= handle_count`. A handle is a pointer plus
a table slot. `ObfDereferenceObject` on 1→0 **and** `handle_count==0`
calls the type's `delete` procedure. If `PERMANENT`, 1→0 is a no-op
until `ObMakeTemporaryObject`.

`delete` procedures do not take locks ranked below `OB`. They may not
allocate. They may signal waiters (the dispatcher already did that when
the object became signaled).

## Types v1

| Type | Waitable | Body |
|---|---|---|
| Process | yes (signaled on exit) | aspace, handle table, token, thread list, pid, peb |
| Thread | yes (signaled on exit) | kstack, context, wait blocks, priority, tid, trap frame |
| Section | no | pages or file backing, size, prot |
| File | no | vnode, offset, access, flags |
| Device | no | driver, ext, queue |
| Event | yes | auto/manual, state |
| Mutex | yes | owner thread, recursion, abandoned |
| Timer | yes | due tick, period, dpc |
| Directory | no | 37-bucket hash of name → object |

Why 37: prime, fits two cache lines of pointers plus a lock. We will
not "upgrade to a B-tree" until a directory has > 4k names.

## Handle table

Per-process. v1 is a single 1024-slot array, slot 0 reserved. Handle
value:

```
handle = index << 2     // never 0, never -1, 4-aligned. index starts at 1.

v0.1 encoded `(index << 2) | 4` which aliases index 2 with index 3
(`8|4 == 12 == 12|0`). Caught by NtCreatePipe inserting two handles.
Fixed in v0.2: shift is the tag. Low 2 bits must be 0; `HANDLE_CURRENT`
(`-1`) is still not a table index.
```

`-1` (`0xFFFFFFFFFFFFFFFF`) is the current-process/thread sentinel and
is **not** inserted into the table.

Each slot:

```
entry
  object        referenced pointer, or NULL if free
  access        ACCESS_MASK
  inherit       1 bit
  protect_close 1 bit
  generation    16 bit — bump on close, packed into the high handle bits
                  once we grow past 1024 (not in v1)
```

Lookup: decode index, check slot occupied, check `access & required`,
`ObReferenceObject`. Failure: `STATUS_INVALID_HANDLE` or
`STATUS_ACCESS_DENIED`. Never return a pointer on failure.

Close: if `protect_close`, `STATUS_ACCESS_DENIED`. Else drop handle
count, drop pointer, free slot.

On process exit: walk the table, close everything. Threads of the
process are first put in a rundown; waiters are abandoned.

## Access masks (subset)

```
GENERIC_READ        0x80000000
GENERIC_WRITE       0x40000000
GENERIC_EXECUTE     0x20000000
GENERIC_ALL         0x10000000
DELETE              0x00010000
READ_CONTROL        0x00020000
WRITE_DAC           0x00040000
SYNCHRONIZE         0x00100000
PROCESS_TERMINATE   0x0001
PROCESS_VM_READ     0x0010
PROCESS_VM_WRITE    0x0020
PROCESS_CREATE_THREAD 0x0002
THREAD_TERMINATE    0x0001
FILE_READ_DATA      0x0001
FILE_WRITE_DATA     0x0002
FILE_APPEND_DATA    0x0004
FILE_EXECUTE        0x0020
DIRECTORY_QUERY     0x0001
EVENT_MODIFY_STATE  0x0002
MUTEX_MODIFY_STATE  0x0001
```

`GENERIC_*` is expanded through the type's generic mapping at handle
creation. v1 mappings are hardcoded in `ob/type.c`.

## Namespace

```
\                       root Directory
\ObjectTypes            type objects
\BaseNamedObjects       events, mutexes, sections
\Devices                device objects
\??                     DOS-device directory (RamDisk, Serial0)
\Sessions               unused v1
```

Path walk: `\??\RamDisk\bin\sh` → `\??` → `RamDisk` (symlink-like
mount) → filesystem path `/bin/sh`. v1 has no real symbolic-link object;
`\??\RamDisk` is a Device whose parse procedure hands off to VFS.

Max path: 512 bytes UTF-8. Max component: 64. Depth: 32. Exceed:
`STATUS_OBJECT_NAME_INVALID` (we reuse `STATUS_INVALID_PARAMETER`).

## Waitable objects

Dispatcher header:

```
signal_state    i32   (event 0/1, mutex 1=free, thread 0 until exit)
wait_list       list of wait_block
type            EventNotification / EventSynchronization / Mutant / ...
lock            ranked WAIT
```

`KeWaitForSingleObject`:

1. Lock dispatcher.
2. If signaled and the wait is satisfied (auto-reset event consumes,
   mutex unsignaled and owner=self++recursion), return `SUCCESS`.
3. Else enqueue wait_block on both the object and the thread, set
   thread state `WAITING`, drop dispatcher, `sched_reschedule`.
4. On wake, status is `SUCCESS`, `TIMEOUT`, or `ABANDONED`.

No spinlock is a mutex. A mutex that you can take at `DISPATCH_LEVEL`
is a bug. Mutexes are waitable; they sleep.

## Failure modes

| Bug | What happens |
|---|---|
| Double close | generation/slot empty → `INVALID_HANDLE` |
| Use after process exit | table is gone; the handle is meaningless in the dead process |
| Object name collision | `OBJECT_NAME_COLLISION` |
| Wait on non-waitable | `INVALID_PARAMETER` |
| Mutex release by non-owner | `MUTANT_NOT_OWNED` (`0xC0000046`) |
| Pointer count underflow | panic `"ob ref"` — we do not wrap |

## Reversal log

none. Considered a handle table tree like Windows (3-level). Rejected
until we have processes with > 1k handles; the array is honest.

T6: handle value is `(generation << 16) | (index << 2)`. Generation is
`u32` (32 bits in the handle). Slot 0 is reserved. Insert skips a wrap
to generation 0 so a never-issued handle does not match. `ht_lookup_ex`
snapshots the granted access under the table lock so `ht_duplicate`
does not TOCTOU the access mask. `object_type.close_fn` runs from
`ht_close` — pipes decrement writer/reader counts and signal EOF.

T10: `Ramdisk0` is a Device object with IRP_MJ_CREATE/CLOSE/READ/WRITE.
`/dev/ram0` is a VNODE_BLOCK whose `device` pointer is that object.
Bytes live in a 1 MiB kernel backing store, not in the vnode `data`
blob. Serial0 is still write-only CHAR.

T12: mutex ownership is a list on the thread, not a hope. Death
walks `owned_mutexes`, marks abandoned, wakes one waiter with
`STATUS_ABANDONED` (or leaves `signal_state=1` if nobody waits).
Handle inherit: `ht_insert_ex` / `ht_set_inherit` / `ht_inherit_table`.
`NtCreateProcess` copies inherit-marked slots after stdio seed.
`NtDuplicateObject` requires `PROCESS_DUP_HANDLE` on foreign process
handles, keeps both process refs until the copy finishes, honours
`DUPLICATE_CLOSE_SOURCE | SAME_ACCESS | INHERIT`. `ht_destroy` drops
the table lock before `close_fn` / `ob_dereference` (HEAP is rank 3).
Process and thread dispatcher objects are signaled on the way to
TERMINATED; host selftest waits on both.

T15: `/bin/echo` is no longer a Process-spawned kernel builtin. The
object is still a Process; the image is an ELF section map like
`/bin/hello`. sh's `echo` command remains a ring-0 builtin (standard
shell). Token is still a Process field, not an object.



