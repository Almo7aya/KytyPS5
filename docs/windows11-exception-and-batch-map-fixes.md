# Windows 11 exception-resume and guest batch-map fixes

## Purpose and scope

This document records the failures investigated on the `windows11-fix` branch based at source
revision `96bec13`, the root cause of each failure, the source changes used to correct them, and
the reasoning behind those changes.

The fixes covered here are:

1. Correct the x86-64 machine code in the Windows exception-resume trampoline.
2. Preserve the complete Windows exception context, including AVX extended state.
3. Assert the trampoline data layout at compile time.
4. Flush the instruction cache after generating trampoline code.
5. Clarify the guarded-restore policy message.
6. Allow `KernelBatchMap2` entries with a null address hint to request kernel-chosen placement.

The relevant implementation files are:

- `src/common/hostException.cpp`
- `src/kernel/memory.cpp`

`src/loader/runtimeLinker.cpp:1046` appears in several crash reports, but it is not the origin of
these faults. That line is the common fatal-error reporter used after the runtime linker decides
that a host access violation cannot be resolved.

## Background: why Kyty handles host exceptions

Kyty executes guest x86-64 code directly on the host CPU where possible. Guest memory is also
used by the GPU resource tracker. Some guest pages are intentionally protected so that a CPU read
or write generates a host access violation. The exception handler can then synchronize or
invalidate the corresponding GPU resource, update page protection, and retry the faulting guest
instruction. This uses Windows
[vectored exception handling](https://learn.microsoft.com/en-us/windows/win32/debug/vectored-exception-handling):
the handler receives the captured exception and processor context and can request that execution
continue after Kyty resolves the fault.

A handled fault therefore is not necessarily a game crash. The normal sequence is:

```text
Guest instruction accesses a protected page
                  |
                  v
Windows raises an access violation
                  |
                  v
Kyty's vectored exception handler identifies the tracked page
                  |
                  v
Kyty resolves the protection/coherency condition
                  |
                  v
Windows restores the interrupted CPU context
                  |
                  v
The same guest instruction is retried successfully
```

This path must preserve the interrupted guest state exactly. It is effectively an invisible pause
in the middle of an instruction stream. Any changed general register, stack pointer, flags value,
XMM register, or YMM register can alter game behavior after the handler returns.

## Windows 11 guarded context restoration

Recent Windows 11 builds can route a vectored-exception continuation through a guarded context
restore path. That path validates the continuation stack pointer before resuming execution.

Kyty guest code does not use the ordinary Windows thread stack while it is executing. Its guest
`RSP` points into a guest stack allocation. Consequently, restoring the guest context directly can
fail Windows' host-stack validation even though the guest stack is valid for the emulator. The
failure can terminate the process with `0xc0000409` / `FAST_FAIL_INVALID_SET_OF_CONTEXT` before the
guest instruction is resumed.

The branch introduced a per-thread resume trampoline. The exception dispatcher first restores a
context containing:

- A host-stack `RSP` that passes the Windows continuation check.
- A `RIP` pointing to the trampoline.
- All other registers containing their original guest values.

The trampoline then changes only `RSP` and transfers control to the saved guest `RIP`.

```text
Handled exception context
  guest RIP ------------------------------+
  guest RSP -------------------------+     |
                                      |     |
Exception filter                      |     |
  trampoline.rsp_slot = guest RSP <---+     |
  trampoline.rip_slot = guest RIP <---------+
  context.RSP = host-stack address
  context.RIP = trampoline.code
                  |
                  v
Windows restores a host-valid continuation context
                  |
                  v
Trampoline exchanges host RSP with saved guest RSP
                  |
                  v
Trampoline jumps to saved guest RIP
                  |
                  v
Guest instruction retries with its original register state
```

## Fix 1: malformed trampoline encoding caused `Write [0x8]`

### Failure signature

The initial branch build crashed a few seconds after launching any game with:

```text
Access violation: Write [0000000000000008]
in src/loader/runtimeLinker.cpp:1046
```

The stack trace also ended in a newly allocated executable address. That address was the generated
trampoline page.

### Intended instruction

The trampoline needed to load the saved guest stack pointer without changing a temporary general
register. The intended instruction was a RIP-relative memory exchange:

```asm
xchg rsp, qword ptr [rip + displacement]
```

Before the instruction:

- `RSP` contains the temporary host stack pointer accepted by Windows.
- The memory slot contains the original guest `RSP`.

After the instruction:

- `RSP` contains the original guest stack pointer.
- The memory slot contains the temporary host stack pointer.

`xchg` does not modify arithmetic flags, and this form does not consume a scratch general-purpose
register.

### Incorrect bytes

The original generated bytes were:

```text
48 87 24 25 08 00 00 00
```

They were documented as RIP-relative, but they do not encode RIP-relative addressing.

Byte-by-byte decoding:

| Bytes | Meaning |
|---|---|
| `48` | `REX.W`, selecting a 64-bit operand |
| `87` | `XCHG r/m64, r64` |
| `24` | ModR/M: register is `RSP`; memory operand uses a SIB byte |
| `25` | SIB: no index and no base; a 32-bit absolute displacement follows |
| `08 00 00 00` | Absolute displacement `0x00000008` |

The resulting instruction was effectively:

```asm
xchg qword ptr [0x0000000000000008], rsp
```

Because `xchg` writes both operands, it attempted to write the host stack pointer to address `0x8`.
That exactly explains the reported access type and address:

```text
Access violation: Write [0000000000000008]
```

The exception occurred inside the mechanism that was supposed to resume a previously handled
exception. The second exception was not a valid tracked-memory event, so the runtime linker
reported it as fatal.

### Correct bytes

The corrected instruction is:

```text
48 87 25 09 00 00 00
```

The ModR/M byte is now `25`. With `mod=00` and `r/m=101` in 64-bit mode, the memory operand is
RIP-relative. The instruction is seven bytes long, so its next `RIP` is trampoline offset `0x07`.
The saved stack slot is at offset `0x10`:

```text
target offset - next RIP offset = 0x10 - 0x07 = 0x09
```

Therefore the required displacement is `+0x09`.

A one-byte `nop` keeps the following jump at offset `0x08`. The final trampoline layout is:

| Offset | Size | Contents |
|---:|---:|---|
| `0x00` | 7 | `xchg rsp, qword ptr [rip+0x09]` |
| `0x07` | 1 | `nop` |
| `0x08` | 6 | `jmp qword ptr [rip+0x0a]` |
| `0x0e` | 2 | unused zero padding |
| `0x10` | 8 | saved guest `RSP` slot |
| `0x18` | 8 | saved guest `RIP` slot |

The jump calculation is also exact. The jump begins at `0x08` and is six bytes long, so its next
`RIP` is `0x0e`. The saved guest instruction pointer is at `0x18`:

```text
0x18 - 0x0e = 0x0a
```

Thus these bytes correctly reach the `rip_slot`:

```text
ff 25 0a 00 00 00
```

### Result

The deterministic cross-game `Write [0x8]` failure disappeared after correcting the addressing
mode. This result directly validates the byte-level diagnosis because the incorrect instruction
itself necessarily accessed address `0x8`.

## Fix 2: preserve `CONTEXT_XSTATE` and AVX registers

### Previous behavior

After saving the guest `RSP` and `RIP`, the new Windows 11 path rewrote `ContextFlags`:

```cpp
context->ContextFlags = (context->ContextFlags & ~0x50u) | 0x100000Fu;
```

The mask `0x50` clears two Windows context categories:

| Bit | Windows context category | State affected |
|---:|---|---|
| `0x10` | `CONTEXT_DEBUG_REGISTERS` | Hardware debug registers |
| `0x40` | `CONTEXT_XSTATE` | Extended processor state, including AVX state |

The replacement flags requested control, integer, segment, and traditional floating-point state.
Traditional AMD64 floating-point context contains XMM state, but it does not represent all extended
state. In particular, the upper halves of YMM registers used by 256-bit AVX instructions are part
of XSTATE.

### Why clearing XSTATE is unsafe

A guest page fault can occur at any instruction, including in the middle of an AVX copy, vector
calculation, decompression loop, or shader-data preparation path.

Consider an AVX instruction that faults while accessing a protected guest page:

```text
1. Guest code has valid data in all 256 bits of YMM0-YMM15.
2. A memory operand touches a protected GPU-tracked page.
3. Windows captures the full context, including XSTATE.
4. Kyty resolves the page protection.
5. The resume path clears CONTEXT_XSTATE.
6. Windows restores only the reduced context.
7. The guest instruction is retried without guaranteed YMM upper halves.
```

This is not necessarily followed by an immediate exception. The damaged vector value can first be
stored into a guest object, copied into a descriptor table, used to calculate a pointer, or consumed
by game logic. The eventual failure may therefore look unrelated to the exception that caused the
state loss.

Observed later signatures included:

```text
Access violation: Read [0000000000000020]
Access violation: Read [0000000000000008]
```

Symbolication of the host-side trace showed that `0x1401ea27d` was in runtime shader descriptor
materialization. Disassembly showed a direct read through a computed SRT address that had become the
invalid low address `0x20`. The other failure occurred in guest code while dereferencing a
null-derived address. These are consistent with an earlier state or pointer corruption rather than
with the runtime linker's error-reporting line.

This association is supported by the following evidence:

- The failures appeared across unrelated games after the new Windows resume path was introduced.
- The resume path was the common cross-game change.
- The path explicitly discarded extended state on every handled fault.
- The failures were intermittent, as expected when they depend on the exact instruction interrupted.
- They stopped in user testing after the original context flags were preserved.

The exact downstream instruction affected in every game was not captured, so the relationship to
each later low-address read is a strongly supported diagnosis rather than the same byte-for-byte
proof available for the original `Write [0x8]`.

### Correct behavior

The fix removes the `ContextFlags` rewrite entirely.

The exception's original [`CONTEXT`](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context)
structure already describes which state Windows captured. Kyty changes only the continuation `RIP`
and `RSP`; it must not reduce the set of state Windows restores. Preserving the original flags
keeps:

- General-purpose registers.
- `RIP`, `RSP`, and `RFLAGS`.
- Segment state.
- X87 and XMM state.
- AVX/YMM extended state when it was present in the captured context.
- Any other extended state Windows associated with that exception context.

The trampoline itself uses `xchg`, `nop`, and an indirect `jmp`. It does not consume an XMM/YMM
register or modify arithmetic flags, so a full context restore reaches the guest instruction without
additional register damage.

### Result

After preserving `CONTEXT_XSTATE`, repeated user tests no longer reproduced the random low-address
access violations in the affected non-GTA games. This is the most important general cross-game
correctness fix after the trampoline encoding correction.

## Fix 3: compile-time trampoline layout validation

The generated instructions use hard-coded displacements to fields inside `Trampoline`. Those
displacements are correct only if the structure layout remains:

```text
code     at 0x00
rsp_slot at 0x10
rip_slot at 0x18
```

Two assertions now enforce the data-field offsets:

```cpp
static_assert(offsetof(Trampoline, rsp_slot) == 0x10);
static_assert(offsetof(Trampoline, rip_slot) == 0x18);
```

Without these assertions, a future alignment change, inserted field, or type change could move a
slot while leaving the raw instruction displacement unchanged. The resulting code would read or
write the wrong memory even though it still compiled successfully.

The assertions convert that class of runtime corruption into a compile-time failure at the source of
the mismatch.

## Fix 4: flush generated instructions before execution

The trampoline is written into memory allocated with executable protection. After copying the raw
instruction bytes, the code now calls:

```cpp
FlushInstructionCache(GetCurrentProcess(), tramp->code, sizeof(tramp->code));
```

Windows requires applications that generate executable code dynamically to ensure instruction-cache
coherency after the bytes have been written and before executing them. This is the purpose of
[`FlushInstructionCache`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-flushinstructioncache),
and the same requirement is also called out for executable regions in the
[`VirtualAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
documentation.

x86-64 systems normally provide strong instruction/data cache coherency, so the missing flush was
not the explanation for the exact `Write [0x8]` failure. The decoded incorrect bytes already prove
that failure's cause. The flush is nevertheless required for a correct and portable Windows JIT-like
code generation path, and prevents a processor from executing stale instruction bytes.

Only the `code` array needs instruction-cache flushing. Later changes to `rsp_slot` and `rip_slot` are
ordinary data updates consumed by the generated instructions.

## Fix 5: clarify the guarded-restore policy message

### What the policy code does

During host-exception handler installation, the Windows path obtains the process-local address of:

```text
ntdll!LdrSystemDllInitBlock
```

It temporarily changes protection on the byte at offset `0x9c`, sets bit zero, restores the original
protection, and logs the resulting value.

The intent of this branch-specific workaround is to bypass guarded-restore stack-pointer validation
that rejects Kyty's guest continuation stack model. It applies to the current emulator process. It
does not permanently modify Windows or another process, and it disappears when the emulator exits.

### Why the old message was confusing

The successful message originally said:

```text
host_exception: guarded-restore RSP validation disabled (policy byte=0x01)
```

The word `disabled` described the validation state, but users reasonably interpreted the entire line
as a disabled feature or failure report. In fact, `policy byte=0x01` was the expected success state
for this workaround.

The message now says:

```text
host_exception: guarded-restore workaround active (policy byte=0x01)
```

This is only a wording change. It does not change the bit manipulation or exception behavior.

### Important maintenance caveat

`LdrSystemDllInitBlock+0x9c` is an internal ntdll layout dependency, not a stable public application
interface. Its meaning and offset can be version-sensitive. The workaround should therefore be
treated as Windows-build-specific compatibility code and revalidated when Windows changes its
guarded restore implementation.

If a supported mechanism replaces it later, removing this internal policy dependency would be
preferable. Until then, the log line makes activation visible and the separate failure message still
reports `VirtualProtect` errors.

## Fix 6: `KernelBatchMap2` null address hints and `MAP_FIXED`

### Failure signature

After the exception-resume crash was fixed, GTA III Definitive Edition progressed farther and then
terminated with:

```text
Guest abort()
in src/libs/libC.cpp
```

The guest frame included:

```text
0x0000000901837ad3
```

Repository investigation history already associated that exact GTA III site with an Unreal Engine
`LowLevelFatalError` reporting that `sceKernelBatchMap` failed.

`libC.cpp` was not the source of the problem. It implements the guest `abort()` import. The game
called it intentionally after a required kernel mapping operation returned an error.

### Guest mapping semantics

`KernelBatchMap2` processes a list of operations. Relevant map operations are:

- `MAP_OP_MAP_DIRECT`
- `MAP_OP_MAP_FLEXIBLE`

Each entry contains a `start` field. Its meaning depends on whether a concrete address was supplied:

| `entry->start` | Meaning |
|---|---|
| Non-null | Map at this explicit guest address when fixed mapping is requested |
| Null | No fixed address was requested; select a suitable guest address and return it |

The guest fixed-map flag is `0x10`.

### Previous behavior

`KernelBatchMap`, and some `KernelBatchMap2` callers, pass the fixed-map flag for the batch. The old
implementation forwarded the same flags to every map operation:

```cpp
KernelMapNamedDirectMemory(..., flags, ...);
KernelMapNamedFlexibleMemory(..., flags, ...);
```

For an entry whose `start` was null, this produced a contradictory request:

```text
Choose an address for me, but map it fixed at address zero.
```

Address zero is not a valid fixed guest mapping. The mapping failed with `ENOMEM` (observed as guest
error `0x8002000c`), Unreal treated the failed batch operation as fatal, and the guest called
`abort()`.

### Correct per-entry flags

The fix derives flags separately for each entry:

```cpp
constexpr int GUEST_MAP_FIXED = 0x10;
const int map_flags = entry->start == nullptr ? (flags & ~GUEST_MAP_FIXED) : flags;
```

The direct and flexible map calls receive `map_flags` rather than the batch-wide `flags`.

Behavior after the change:

| Entry | Effective behavior |
|---|---|
| Null start plus batch `MAP_FIXED` | Clear `MAP_FIXED`; choose an address and store it in `entry->start` |
| Explicit start plus batch `MAP_FIXED` | Preserve `MAP_FIXED`; map at the requested address |
| Entry without `MAP_FIXED` | Unchanged |
| Unmap/protect/type-protect operation | Unchanged; `map_flags` is not used by those operations |

This preserves fixed-map semantics for every caller that actually provides an address. It changes
only the impossible null-address fixed-map combination.

### Relationship to `main`

The batch-map defect was latent in `main` as well as this branch. The branch's other changes allowed
GTA III to progress to the path that exposed it consistently. Therefore:

- The malformed trampoline and XSTATE reduction were branch-specific regressions.
- The GTA batch-map error was an existing kernel emulation bug exposed during branch testing.

### Result

User testing after applying the per-entry flag fix no longer reproduced the GTA III guest abort at
the known mapping failure point.

## Why the reported source lines were misleading

The crash reporter records the line that converts an unresolved condition into a fatal report. It
does not necessarily record the line containing the instruction that originally faulted.

| Reported location | Actual meaning in these incidents |
|---|---|
| `runtimeLinker.cpp:1046` | Runtime linker formatted an unresolved access violation |
| `libC.cpp:254` | Guest called the imported `abort()` function |
| Trampoline allocation address in stack | Generated resume code was executing |
| Guest address such as `0x901837ad3` | Guest call site or stack evidence requiring game/module interpretation |

Diagnosis required combining the access type and address, branch diff, generated instruction bytes,
PDB symbolication, disassembly, and existing repository investigation history.

## Failure-to-fix mapping

| Symptom | Diagnosis | Fix |
|---|---|---|
| Deterministic `Write [0x8]` after the first handled fault | SIB encoding selected absolute address `0x8` instead of RIP-relative `rsp_slot` | Replace malformed bytes with `48 87 25 09 00 00 00`, retain jump at offset `0x08` |
| Random cross-game low-address reads after some execution | Strongly supported: resume path reduced the captured context and discarded AVX XSTATE | Preserve the original `ContextFlags` |
| Potential stale generated code | Missing explicit instruction-cache synchronization | Call `FlushInstructionCache` after writing code |
| Future structure edit silently breaks raw displacements | Generated code depended on unverified field offsets | Add `offsetof` static assertions |
| Successful policy activation looked like an error | Ambiguous wording around “validation disabled” | Log “workaround active” |
| GTA III `Guest abort()` at the known batch-map failure point | Null address hint incorrectly combined with `MAP_FIXED` | Clear fixed mapping per null-start map entry |

## Corrected end-to-end exception flow

The complete handled-fault path after these changes is:

1. Guest or emulator code accesses a protected tracked page.
2. Windows captures the complete processor context, including XSTATE when applicable.
3. Kyty's vectored exception filter constructs `ExceptionInfo` and calls the runtime handler.
4. The runtime handler synchronizes/invalidate resources or updates page protection.
5. If the fault is resolved, the filter acquires the current thread's trampoline.
6. The original guest `RSP` is stored at trampoline offset `0x10`.
7. The original guest `RIP` is stored at trampoline offset `0x18`.
8. The continuation `RIP` is changed to trampoline offset `0x00`.
9. The continuation `RSP` is temporarily changed to an address in the host stack.
10. The original `ContextFlags` remain unchanged, preserving extended state.
11. Windows restores the continuation context.
12. `xchg rsp, [rip+0x09]` loads the guest stack pointer from offset `0x10`.
13. `jmp [rip+0x0a]` loads the guest instruction pointer from offset `0x18`.
14. The faulting instruction retries with its full pre-fault state.

## Validation performed

The source changes were reviewed statically with:

- Branch-to-`main` diffs.
- Manual x86-64 instruction decoding and displacement calculation.
- Compile-time layout assertions added to the source.
- `git diff --check` for patch formatting.
- Symbolication of existing crash addresses using the already-built executable and PDB.
- Disassembly of the host fault site without launching the emulator.
- Comparison with historical GTA III investigation commits for the exact guest abort address.

At the user's request, no project build or emulator execution was performed by the person preparing
the source changes. Runtime validation was performed by the user.

Observed user validation:

- The original `Write [0x8]` crash disappeared.
- GTA III no longer reproduced the known guest batch-map abort.
- Random access violations in other tested games stopped after the full-context fix.

Because the later failures were intermittent, useful continued coverage includes:

1. Several cold launches of multiple games.
2. Loading transitions that trigger heavy GPU resource tracking.
3. Games with AVX-heavy copy/decompression paths.
4. Long sessions that create and destroy many guest threads.
5. Repeated GTA III menu-to-gameplay transitions.

## Remaining design considerations

These fixes correct the observed regressions, but the Windows continuation implementation still has
maintenance considerations:

- The guarded-restore policy byte depends on an undocumented ntdll layout.
- Trampolines use executable/writable memory because their slots are updated for each continuation.
- The implementation keeps a fixed table of 64 per-thread trampolines.
- Trampoline allocations persist for the process lifetime.
- The current lazy trampoline acquisition path can take a mutex and allocate memory while handling
  an exception. Microsoft's
  [vectored exception callback guidance](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nc-winnt-pvectored_exception_handler)
  recommends avoiding synchronization and allocation in a handler. Preallocating trampolines or
  initializing per-thread continuation state before guest execution would be a useful hardening
  step. This was not the cause of the failures diagnosed here.
- Windows updates that change guarded context restoration should be tested explicitly.

None of these considerations changes the diagnosed causes above, but they are useful boundaries for
future hardening work. A future redesign should retain the central invariant established by this
investigation: a handled guest fault must resume with every observable guest register and memory
semantic preserved exactly.

## External references

- [Microsoft: Vectored Exception Handling](https://learn.microsoft.com/en-us/windows/win32/debug/vectored-exception-handling)
- [Microsoft: Vectored exception handler callback](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nc-winnt-pvectored_exception_handler)
- [Microsoft: x64 `CONTEXT` structure](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-context)
- [Microsoft: `FlushInstructionCache`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-flushinstructioncache)
- [Microsoft: `VirtualAlloc`](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)

There is intentionally no public Microsoft reference for `LdrSystemDllInitBlock+0x9c`. It is an
internal implementation detail; that lack of a supported contract is why the policy-byte workaround
is documented above as version-sensitive compatibility code rather than a general Windows API.
