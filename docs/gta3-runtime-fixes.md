# GTA III Definitive Edition runtime fixes

This document describes the five commits on `fix/gta3-runtime-fixes`. The commits were kept
separate so each failure mode can be reviewed, tested, bisected, or reverted independently.

The reproducer used throughout the investigation was:

```powershell
_Build\windows\kyty_emulator.exe `
  --printf-direction Silent `
  --game Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin
```

Only the `kyty_emulator` target was built. The launcher was not needed for these fixes.

## Commit overview

| Commit | Area | Failure addressed |
| --- | --- | --- |
| `5a2dabf` | Guest/host image formats | `unknown format: format = 34` in `tile.cpp` |
| `1f0f3fb` | Windows guest-code patching | Corrupted zlib `inflate_fast` state after a tracked-memory page fault |
| `5b080c0` | Storage-buffer descriptors | Oversized reads and `invalid memory tracker range` |
| `3bf05d7` | Kernel virtual memory | Reads or writes into reserved but uncommitted guest pages |
| `8c425c5` | Vulkan sampler cache | `unknown ratio: 7` in `samplerCache.cpp` |

The fixes form a progression through the same GTA III startup and level-streaming path. Removing
an earlier blocker exposed the next one, but the implementations are not intentionally coupled.

## 1. `5a2dabf` — support the 11:11:10 unsigned texture format

### Original failure

GTA III created a surface with this descriptor:

```text
format = 34
width  = 1680
height = 946
pitch  = 1680
levels = 1
tile   = 27
```

`TileGetTextureSize` eventually terminated with `unknown format`. Format value 34 is not malformed:
`BufferFormat::k11_11_10UInt` is explicitly assigned value 34 in `gpu_defs.h`.

### Root cause

Kyty keeps format properties in the sparse `kFormatInfo` table in
`src/graphics/guest_gpu/gpu_format.cpp`. The table drives several decisions:

- bytes per element for linear and tiled size calculations;
- compressed-block size, when applicable;
- render-target element size;
- whether a format can be sampled;
- whether the host shader path should treat the sampled value as an unsigned integer.

The enum contained `k11_11_10UInt`, but `kFormatInfo` did not. Consequently,
`FindFormatInfo(34)` returned null and `NumBytesPerElement(34)` returned zero. The tiler could not
derive a valid block layout from the remaining metadata and rejected the format.

There was a second missing piece. Even if the tiler knew the element size, host image creation
would still need a Vulkan format. Vulkan exposes `VK_FORMAT_B10G11R11_UFLOAT_PACK32`, but it does
not expose an unsigned-integer image format with an 11:11:10 component split.

### Implementation

The commit adds this format description:

```cpp
{GpuEnumValue(BufferFormat::k11_11_10UInt), 4, 0, 4, true, false}
```

The fields mean:

- `4`: one element occupies 32 bits;
- `0`: it is not a block-compressed format;
- `4`: render-target size calculations also use four bytes per element;
- `true`: the sampled-texture path is supported;
- `false`: use the non-integer host sampling path because the selected Vulkan view is a float
  format.

The corresponding host mapping is:

```cpp
{Prospero::BufferFormat::k11_11_10UInt, vk::Format::eB10G11R11UfloatPack32}
```

The two formats have the same 32-bit component packing, so allocation sizes, pitches, tile
addresses, copies, and raw memory ownership remain correct. The host interprets those component
bits as unsigned floating-point values because that is the compatible Vulkan image format.

### Why this is narrowly scoped

- No existing format entry was modified.
- Only enum value 34 gets the new size and Vulkan mapping.
- The change does not alter tiling formulas; it supplies the missing element metadata to the
  existing formulas.
- The Vulkan mapping is explicit, so unsupported formats still fail normally rather than being
  assigned a generic fallback.

### Compatibility limitation

The Vulkan view is a representation-compatible approximation, not exact unsigned-integer numeric
semantics. It is appropriate for GTA III's observed sampling use, but a title that depends on true
integer arithmetic for this packed format may need shader-side unpacking or a different emulation
strategy. Marking the format as an integer texture while binding a floating-point Vulkan view would
instead create a shader/view type mismatch.

### Files changed

- `src/graphics/guest_gpu/gpu_format.cpp`
- `src/graphics/host_gpu/vulkanCommon.cpp`

## 2. `1f0f3fb` — avoid zlib red-zone corruption on Windows

### Original failure

During decompression, guest zlib code entered `inflate_fast`. Its output first touched memory whose
CPU access was being tracked through page protection. The resulting host page fault was handled and
execution resumed, but a live zlib local was corrupted. The observed consequence was a later store
through a null destination pointer rather than a clean failure at the original page fault.

This was particularly misleading because the failing instruction was valid guest code and the
tracked-memory fault itself appeared to have been handled successfully.

### ABI interaction behind the corruption

PS5 guest code follows the SysV AMD64 ABI. SysV gives leaf functions a 128-byte **red zone** below
`RSP`; a function may store temporary values there without first subtracting from `RSP`.

Windows x64 has no equivalent red-zone guarantee. A Windows exception is delivered using the
interrupted thread and stack. Kyty's host exception machinery and the Windows exception-dispatch
path are therefore allowed to use memory immediately below the interrupted `RSP`.

The problematic `inflate_fast` implementation kept live state in the SysV red zone. A protected
output-page access transferred control through the Windows exception path, which could overwrite
that state. When guest execution resumed, zlib continued with corrupted pointers.

### Why the fix selects zlib's slow path

The same zlib routine already contains a slow decompression path. Unlike `inflate_fast`, that path
keeps its state in the caller's allocated stack frame. It produces the same decompressed byte stream
and avoids relying on red-zone contents across a Windows page fault.

`PatchProgram` now scans loaded guest program ranges for a byte-exact sequence surrounding the
unsafe fast-path call. When it finds the sequence, it changes byte 7:

```text
0x72  JB rel8     ->  0xEB  JMP rel8
```

The relative displacement is left unchanged. The original conditional branch already targets the
slow path; changing only the opcode makes that branch unconditional.

### Patch-safety properties

- The code is compiled only when `KYTY_PLATFORM` is Windows.
- Matching requires the complete instruction sequence, not a short or ambiguous opcode prefix.
- If a different zlib build or compiler produces different code, no bytes are changed.
- The scan checks the program size before subtracting the pattern length.
- After a match, the iterator skips the rest of the matched sequence so overlapping matches are not
  reconsidered.
- A log entry records the address of every patched call site.
- Linux and macOS retain the original fast path because their signal/ABI behavior is different.

### Tradeoff

Decompression can be slower on Windows because the optimized inner loop is bypassed. This is a
targeted correctness tradeoff: it affects only guest code containing the exact known call sequence,
and only on Windows. It does not globally disable zlib acceleration in the host process.

### Maintenance consideration

This is intentionally a binary compatibility patch. If the game or embedded zlib code changes, the
pattern may stop matching. A longer-term architectural solution would preserve the guest SysV red
zone across Windows exception delivery, which would remove the need for function-specific patches.

### File changed

- `src/loader/runtimeLinker.cpp`

## 3. `5b080c0` — clamp storage buffers to mapped guest memory

### Original failures

The same underlying issue produced several outwardly different failures:

```text
Access violation: Read [00000011413fffe0]
Access violation: Read [0000000004d00000]
invalid memory tracker range
```

The stacks converged on storage-buffer binding and `BufferCache::ObtainBuffer`. Depending on the
descriptor address, mapped layout, and timing, the oversized operation could fail in a stream-buffer
copy or at `MemoryTracker::ValidateRange`.

### Root cause

A guest buffer descriptor contains a 48-bit base address, a 14-bit stride, and a 32-bit record
count. `NativeStorageBuffer` computes its nominal byte footprint as:

```text
stride != 0 ? stride * num_records : num_records
```

GNM descriptors can deliberately over-provision `num_records`, including values close to
`UINT32_MAX`. Guest shaders then perform their own dynamic bounds checks and only touch a small
mapped portion of the nominal descriptor.

Before this commit, Kyty treated the nominal footprint as if all of it were resident. It passed that
size through buffer-cache lookup, upload/readback, memory tracking, and finally the Vulkan descriptor
range. That could cross an unmapped boundary or the GPU tracker's one-terabyte address-space limit,
even though the shader's real accesses were valid.

### New range query: `RangeSet::ContiguousExtent`

`GpuResourceManager` already records guest GPU mappings in a `RangeSet`. `RangeSet::Add` coalesces
overlapping and adjacent ranges, so one map entry represents an entire contiguous run.

`ContiguousExtent(address, max_size)` performs these steps:

1. Reject null addresses and zero maximum sizes.
2. Use `upper_bound(address)` to locate the range immediately before or containing the address.
3. Confirm that the address is inside the half-open interval `[range_start, range_end)`.
4. Return the smaller of:
   - bytes remaining between `address` and `range_end`; and
   - the caller's requested maximum.

If the address is not mapped, it returns zero.

### Thread-safe GPU-facing wrapper

`GpuResourceManager::MappedExtent` wraps the range query and enforces tracker invariants:

- `vaddr` and `max_size` must be nonzero;
- `vaddr` must be below `TRACKER_ADDRESS_SIZE` (`1 << 40`);
- `max_size` is clipped before `vaddr + max_size` could overflow the tracker address space;
- the mapped-range set is read under `m_mapped_ranges_mutex` using a shared lock.

The shared lock matters because guest mappings can change while renderer threads are preparing
descriptors.

### Descriptor binding changes

`NativeStorageBuffer` still calculates and overflow-checks the nominal descriptor size. It then
asks `MappedExtent` how many bytes are actually mapped from the descriptor base.

The returned `bound_size` replaces the nominal size in every operation that can touch host or guest
memory:

- `BufferCache::ObtainBuffer`;
- `maxStorageBufferRange` validation;
- offset-adjustment validation;
- the final `VkDescriptorBufferInfo::range`.

If no byte is mapped at the base, Kyty binds its null storage buffer instead of asking the buffer
cache to materialize an invalid range.

The final Vulkan range includes `adjustment`, which accounts for aligning the bound buffer offset
down to `minStorageBufferOffsetAlignment`. The existing checks still require the adjustment to be
32-bit aligned, smaller than 256 bytes, and small enough that `bound_size + adjustment` fits the
device limit.

### Why clipping is preferable to increasing tracker limits

The descriptor's enormous size is not evidence that the guest allocated an enormous buffer. Raising
the one-terabyte tracker limit or reserving host memory for the nominal footprint would preserve the
incorrect assumption and consume resources for addresses the guest never mapped.

Clipping at the first mapping boundary reflects the available guest address space. Vulkan robust
buffer access is enabled by Kyty, so shader reads beyond the bound range use robust out-of-bounds
behavior instead of causing the host buffer cache to read arbitrary memory.

### Edge cases and limitations

- A descriptor beginning in an unmapped hole becomes a null binding.
- A descriptor spanning two adjacent mappings works because adjacent ranges are coalesced.
- A descriptor spanning a genuine unmapped gap stops at that gap. Later mappings are not combined
  into one Vulkan range.
- The nominal multiplication overflow check remains in place; malformed descriptors do not get
  hidden by clipping.
- This commit changes storage-buffer binding only. Texture, uniform, and address-buffer paths keep
  their own size rules.

### Files changed

- `src/graphics/host_gpu/rangeSet.h`
- `src/graphics/host_gpu/renderer/cache/gpuResourceManager.h`
- `src/graphics/host_gpu/renderer/cache/gpuResourceManager.cpp`
- `src/graphics/host_gpu/renderer/pipeline/descriptors.cpp`

## 4. `3bf05d7` — commit reserved guest pages on demand

### Original failure

After the storage-buffer clamp allowed streaming to continue, GTA III faulted while writing a GPU
aperture address such as:

```text
Access violation: Write [00000020fdf42c00]
```

Windows reported the containing region as `MEM_RESERVE`: the virtual address existed, but no physical
pages were committed. Large guest reservations can cover hundreds of gigabytes while individual
streaming-pool blocks are committed only when needed.

### Previous behavior

The exception handler already tried two specialized paths:

1. `HandleGpuFault` for pages protected by GPU ownership tracking.
2. `KernelHandleReservedRangeAccessViolation` for the existing AMM-specific unsupported-unmap case.

A normal read or write into a non-AMM reserved range matched neither path, so the exception became a
fatal access violation.

### New exception flow

For read and write violations only, `KytyExceptionHandler` now calls
`KernelCommitReservedOnFault(vaddr)` after the two existing handlers. If the function commits the
page successfully, it returns `true`; the host exception layer resumes the exact faulting guest
instruction.

Execute faults are deliberately excluded. Committing writable data memory in response to an execute
fault would hide a different class of guest bug and would weaken the distinction between code and
data mappings.

### Commit granularity

The fault address is aligned down to a 64 KiB block:

```cpp
block = vaddr & ~(0x10000 - 1);
```

64 KiB is the Windows allocation granularity and the unit used by the streaming-pool behavior seen
in GTA III. The guest page size used inside the fallback is 16 KiB (`0x4000`).

When the range table knows the reservation, the block is clamped to the exact reservation bounds.
This handles a reservation whose start or end is not a complete 64 KiB block without consuming
neighboring virtual memory.

### Tracked-reservation path

For a range whose type is `Reserved` or `PoolReserved`, the operation is transactional:

1. Acquire `g_memory_operation_mutex` to serialize the change with map, unmap, protect, and fixed-map
   operations.
2. Query the range metadata and calculate the bounded 64 KiB span.
3. Remove that span from the reserved entries with `ConsumeReservedSpan`.
4. Map zeroed flexible-memory backing at the same virtual address with CPU read/write access and no
   initial GPU access mode.
5. Add a committed `VirtualRangeType::Flexible` entry. `disallow_merge` is set so the independently
   fault-committed block remains an explicit unit for later replacement or unmap operations.
6. Notify the GPU resource manager through `MapGpuRange`.
7. Invoke the registered allocation callback so other memory subsystems see the new committed span.

If flexible-memory mapping fails, the reserved range record is restored. If adding the committed
range record fails, the flexible mapping is unmapped and the reservation record is restored. These
rollback paths prevent the host mapping, flexible-memory allocator, and virtual-range table from
silently disagreeing.

### OS-only reservation fallback

Some large aperture reservations can exist in Windows without a corresponding `VirtualRanges`
entry. For that case the code uses `VirtualQuery` and proceeds only when the operating system reports
`MEM_RESERVE`.

It first asks `GuestAddressSpace::Commit` to replace a placeholder owned by Kyty. If that address is
not on the guest-address-space free list, `CommitFixedHostRange` is used as a fallback.

`CommitFixedHostRange` walks 16 KiB pages and handles three useful states:

- an already committed page is changed to the requested protection;
- a reserved page is committed;
- otherwise, fixed allocation, commit, and protection operations are tried in order.

On non-Windows hosts it first attempts one fixed allocation for the full range, then retains the
page-wise fallback for portability.

### Why both bookkeeping paths exist

The tracked path is preferred because it keeps these systems synchronized:

- guest virtual-range metadata;
- flexible-memory backing allocation;
- host virtual-memory mappings;
- GPU mapped-range tracking;
- allocation callbacks.

The OS-only path is a compatibility fallback for reservations that Kyty did not record. It makes the
faulting memory accessible, but it cannot reconstruct metadata that was never present. Later virtual
queries or complex remaps of such an untracked span may therefore require additional bookkeeping if
another title exposes that behavior.

### Fault-handler safety boundaries

- Null global memory managers cause a normal `false` return rather than partial work.
- The operation is serialized by the same recursive mutex as normal kernel memory operations.
- Only confirmed reserved memory is committed by the untracked Windows fallback.
- Arbitrary free addresses and execute faults are not converted into writable mappings.
- The function returns `true` only after the requested block is accessible.

### Files changed

- `src/kernel/memory.h`
- `src/kernel/memory.cpp`
- `src/loader/runtimeLinker.cpp`

## 5. `8c425c5` — clamp reserved anisotropy encodings

### Original failure

GTA III supplied this four-dword sampler descriptor:

```text
20120f00,c4700000,00ffc0ff,91b00fac
```

The `MAX_ANISO_RATIO` bits decoded to 7, and `SamplerCache::GetSampler` terminated with:

```text
unknown ratio: 7
```

### Root cause

`ShaderSamplerResource::MaxAnisoRatio` extracts a three-bit field, so its encoded domain is 0 through
7. The named `SamplerAnisoRatio` enum defines the normal values only:

| Encoding | Ratio |
| --- | --- |
| 0 | 1x |
| 1 | 2x |
| 2 | 4x |
| 3 | 8x |
| 4 | 16x |

The old switch handled exactly those five enum members and aborted for 5, 6, or 7. GTA III uses 7
for post-processing samplers. The hardware treats encodings above 4 as saturation at the maximum
ratio rather than as a fatal descriptor.

### Implementation

The switch is replaced with a bounded exponent calculation:

```cpp
const auto ratio = static_cast<uint32_t>(r.MaxAnisoRatio());
aniso_ratio = static_cast<float>(1u << (ratio < 4u ? ratio : 4u));
```

This produces:

| Encoding | Vulkan `maxAnisotropy` |
| --- | --- |
| 0 | 1.0 |
| 1 | 2.0 |
| 2 | 4.0 |
| 3 | 8.0 |
| 4, 5, 6, 7 | 16.0 |

The value is used only when either the magnification or minification filter is anisotropic. For
ordinary point or bilinear filtering, `maxAnisotropy` remains 1.0 and anisotropy stays disabled.

Kyty's Vulkan device selection already requires `maxSamplerAnisotropy >= 16`, so the saturated value
does not exceed the supported device limit.

### Cache behavior

The sampler cache key remains the original four descriptor dwords. Consequently, encodings 4, 5, 6,
and 7 can occupy separate cache entries even though they resolve to identical Vulkan anisotropy.
That preserves descriptor identity and keeps the change local to sampler creation; it trades a small
amount of possible cache duplication for predictable behavior.

### Why the value is not simply ignored

Ignoring the field or disabling anisotropy could materially change post-processing sampling. Clamping
to 16x preserves the hardware's saturation behavior and the title's request for maximum anisotropic
quality.

### File changed

- `src/graphics/host_gpu/renderer/cache/samplerCache.cpp`

## Verification history

All verification builds used only this target:

```powershell
cmake --build _Build/windows --target kyty_emulator --parallel
```

The following results were observed with the exact Silent reproducer:

1. After format 34 support, the original `unknown format` failure no longer occurred.
2. After the Windows zlib patch, the previous null write caused by corrupted `inflate_fast` state did
   not recur during a run lasting over one minute.
3. After the mapped-range clamp, neither reported oversized read nor `invalid memory tracker range`
   recurred. The game progressed far enough to expose the reserved-aperture write.
4. After on-demand reserved commits, all earlier target signatures remained absent and the run
   progressed to 954 compiled shaders before encountering a later guest-code failure.
5. After the sampler clamp, the exact run passed the former 954-shader boundary, reached 971 shaders,
   remained responsive during the watch window, and contained zero `unknown ratio` messages. The
   verification instance was then stopped manually.

`git diff --check` passed before the fixes were split into commits and again after the split.

## Known non-goals and remaining failures

These commits fix the five documented blockers; they do not claim that GTA III is fully playable.
Later runs exposed failures with different causes and signatures, including:

- a guest-code read from `0xfffffffffffffff8` at guest instruction `0x0000000900643fa0`;
- a timing-dependent `remaining_dw < 2` command-buffer guard in `graphicsRun.cpp` during a verbose
  diagnostic run.

Those failures occur after the fixed paths and should be investigated separately rather than folded
into these commits.

Generated runtime data and logs (`_DownloadData`, `_SaveData`, `_Shaders`, and `_gta3_logs`) are not
part of the branch. The pre-existing `3rdparty/tracy` worktree modification is also unrelated and was
not committed.

## Reviewing or reverting the series

Show the complete series relative to `main`:

```powershell
git log --oneline main..fix/gta3-runtime-fixes
git diff main..fix/gta3-runtime-fixes
```

Inspect one change in isolation:

```powershell
git show 5a2dabf
git show 1f0f3fb
git show 5b080c0
git show 3bf05d7
git show 8c425c5
```

Each commit includes all declarations and call sites required for its own fix. There are no known
mandatory revert-order dependencies between the five commits, although reverting an earlier blocker
can prevent runtime testing from reaching later code.
