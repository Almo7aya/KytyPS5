# GTA III: The Definitive Edition (PPSA03527) — Engineering Worklog (Session 2)

**Audience:** the next agent, with zero memory of this session.
**Branch:** `fix/gta3-menu-start-crash` (base commit `25521a5`).
**Rule of this document:** every claim is backed by something observed (log line, measurement, dump, diff, test). Anything not directly verified is marked **UNVERIFIED** or **HYPOTHESIS**. Supersedes nothing in `docs/gta3-definitive-issues.md`; read that first for session-1 history.

---

## 1. Current status (fact)

**Starting point of this session (from `docs/gta3-definitive-issues.md` §0/§2):** menu loads at ~9–11 fps, Start accepted, ~497 shaders compile, then the run **deterministically froze** in the pixel-shader recompiler (GpuWorker `state=shader_ps`, `act` frozen ≥240 s, `Num compiled` stuck at 497). ~40% of runs instead hit a timing-dependent `Guest abort()` at ~10–14 s past Start.

**Where the game gets today (this session's runs, all with the working-tree build):**

| Run | Outcome | Evidence |
|---|---|---|
| precmp | Hung in recompiler at PS `0x206b920000` (pre-fix build) | WAITWATCH `state=shader_ps arg0=0x206b920000 held_ms` climbing, `act` frozen |
| fix1 | `Guest abort()` at t=10.4 s past Start, 451 shaders | `libC.cpp:253`, stack `[2] 0x0901837ad3` |
| fix2 | Fatal EXIT `memory tracker re-entered from upload callback` during intro, frame 244 | `memoryTracker.h:131` (old numbering) |
| fix3/fix4 | `Guest abort()` (t=10.4 s / t=17.3 s, 446 shaders) | same abort site |
| fix5 | Fatal EXIT `unsupported GPU invalidation alias` (depth target, raw GPU write) | `textureCache.cpp:3695` (old numbering) |
| fix6 | `Guest abort()`, 448 shaders | same abort site; zero-fill verdicts all `no` |
| fix7 | Quiescent stall (deadlock) at frame 259, no crash, killed at watch end | WAITWATCH: worker `cp_fault` 7.7 s, others `gpu_pause_lock`/`gpu_idle` |
| fix8 | `Guest abort()` at t=13.8 s | same abort site; zero-fill verdicts all `no` |
| fix9 | Slow grind (~0.2 fps), 288 shaders, no crash, killed at watch end | worker actively in `gpu_pm4`/`draw_prepare` |
| fix10 | `Guest abort()` | same abort site |
| coal2 | Fatal EXIT `host write aliases unsupported image` (depth target) | `textureCache.cpp:3365` |
| coal3–coal6 | Quiescent stall (readback deadlock, see §4) | WAITWATCH dumps |
| coal8 | Fatal EXIT `Unknown exception (c000001d)` at `eboot+0x1b0bae5` | `runtimeLinker.cpp:779` |
| coal1 | Drain-coalescing effect measured; then `Guest abort()` | DRAIN/s below |

**Net change (fact):** the shader-recompiler freeze (doc §2) is gone in every run since the fix — no run has hung in `shader_ps` since; runs now progress until one of: the §3.2 `Guest abort()` (~50%), the readback deadlock (~25%), a texture-cache EXIT (all 3 observed variants fixed), the tracker re-entry EXIT (fixed), or the watch timeout (~25%, grinding at ~0.2 fps). **It is UNKNOWN whether the game reaches gameplay** — no run survived past ~17 s of level streaming. Best observed: frame ~294, brief fps 3.8.

**Committed vs. working tree (fact):** NOTHING from this session is committed. Everything lives in the working tree on top of `25521a5`. `git status --porcelain` shows modified: `src/CMakeLists.txt`, `src/common/waitWatch.h`, `src/graphics/guest_gpu/graphicsRun.cpp`, `src/graphics/host_gpu/gpuTiler.cpp`, `src/graphics/host_gpu/memoryTracker.{h,cpp}`, `src/graphics/host_gpu/objects/label.cpp`, `src/graphics/host_gpu/pageManager.cpp`, `src/graphics/host_gpu/regionManager.h`, `src/graphics/host_gpu/renderer/bufferCache.cpp`, `src/graphics/host_gpu/renderer/imageInfo.h`, `src/graphics/host_gpu/renderer/textureCache.cpp`, `src/graphics/shader/recompiler/{ScalarProvenance,ShaderRecompiler}.cpp`, `src/graphics/shader/shader.cpp`, `src/kernel/{memory.h,memory.cpp}`, `src/libs/libC.cpp`, `src/loader/runtimeLinker.cpp`; untracked: `src/common/waitWatch.cpp`, `tests/ShaderRecompileFile.cpp`, `docs/gta3-definitive-issues.md`, `docs/gta3-worklog.md`, `drive_gta3.ps1`, `_work/`, `_gta3_logs/`.

---

## 2. Changes made this session

### 2.1 THE FIX — scalar-provenance fixpoint infinite loop (doc §2)

- **Symptom (observed):** GpuWorker frozen in `state=shader_ps arg0=0x206b920000`, `act` frozen ≥240 s, `Num compiled` stopped (497 in doc runs, varies), all DRAIN counters 0. Doc had localized it to post-decode recompilation but not the loop.
- **Root cause (CONFIRMED by measurement):** `IR::BuildScalarProvenance`'s worklist fixpoint (`src/graphics/shader/recompiler/ScalarProvenance.cpp`, `Builder::Run`, `MergeEntry`) oscillated forever. A temporary probe (since removed) printed, every 65536 block-visits: `values=1133` (stable, no growth) while entries flipped `phi388→phi799→phi657` (reg31) and `phi662→phi804→phi824` (reg42) with a ~393216-visit period. Mechanism: once a phi node existed for a (block, reg) slot, a later merge whose incoming set temporarily collapsed to one value **fell back to the plain value** instead of the phi, making the merge non-monotonic; the header entry then alternated between the propagated phi and the slot's own phi one loop-trip apart — a limit cycle. The loop in question: the inner streaming loop at CFG blocks 16–19 (`s30` counter, 4-tap `image_sample_l` body), with phis at blocks 3/15/16/22.
- **Fix (files: `ScalarProvenance.cpp`):** in the `merge` lambda, if `*phi` already exists, always return it (still updating `phi_args`); only fall back to `incoming[0]` before any phi exists. Plus a permanent safety net: `kMaxFixpointIterations = 16M`, beyond which the pass returns a "did not converge" error (→ compile failure → existing `ExitShaderRecompilerFailure` path) instead of a frozen worker.
- **Verification (fact):** the offline harness (§2.3) reproduces the hang 100% pre-fix (watchdog fired after 60 s with phase log stuck after `IR LowerProgram`); post-fix the same shader compiles in **13 ms** (`TryRecompile OK ... spirv_words=205184`, provenance phase 11 ms). In-game: no `shader_ps` stall in any subsequent run (runs reached 288–650 compiled shaders). Full test suite passes except one **pre-existing** failure (§3.4).

### 2.2 Diagnostic: pre-compile GCN shader dump (env-gated)

- **File:** `src/graphics/shader/shader.cpp` — `DumpShaderRecompilerPrecmp`, called before each `TryRecompile` in VS/PS/CS compile functions; active only when env `KYTY_DUMP_PRECMP` is set. Writes `_Shaders/precmp/<seq>_<stage>_<hash>_addr<addr>.bin`.
- **Result (fact):** the last file written before a hang (`0562_ps_00000000afe44e37_addr000000206b920000.bin`, 10432 bytes) matched the WAITWATCH `arg0` exactly — proving the dump-before-compile approach (the existing `DumpShaderRecompilerOriginal` is disabled by an early `return;` and runs *after* compile, so it never fires for a hanging shader).

### 2.3 Diagnostic: offline recompile harness

- **Files:** `tests/ShaderRecompileFile.cpp`, target `shader_recompile_file` in `src/CMakeLists.txt` (EXCLUDE_FROM_ALL). Usage: `shader_recompile_file <shader.bin> [ps|vs|cs] [watchdog_s|dumpcfg]`. Has a watchdog thread that `_Exit(3)` on timeout, and a `dumpcfg` mode printing `GraphToString` + decoded RDNA2.
- **Result (fact):** reproduced the §2 hang in seconds (no game launch) and verified the fix; the CFG dump (`_work/cfg_dump.txt`) is where the loop structure above was read.

### 2.4 Diagnostic: phase LOGF lines in `ShaderRecompiler.cpp`

- Added `phase end` LOGF lines after `BuildScalarProvenance`, `BuildSrtPlan`, `PatchSrtReads+TrackResources`, `MaterializeResources`, `SpecializeResources`, `CollectShaderInfo`, `AllocateBindings`. **Fact:** `Log::Write` is dropped entirely with `--printf-direction Silent` (`log.cpp:118`), which is why the doc never saw these; they print in the harness (Console direction). They pinpointed `BuildScalarProvenance` as the hanging phase.

### 2.5 TextureCache: raw GPU writes over render/depth/storage targets (the "GPU invalidation alias" wall)

- **Symptom (observed, run fix5):** `TextureCache: unsupported GPU invalidation alias, addr=0x20701a0000 size=0x48000 cached_kind=3 cached=0x20701a0000+0x80000 gpu_modified=1 buffer_modified=0 formatted=0 ambiguous=0` (`textureCache.cpp:3695` old). `kind=3` = DepthTarget (enum at `textureCache.cpp:194`). A raw (unformatted) GPU buffer write covered the first half of a GPU-modified depth target's guest range; `ClassifyBufferImageWrite` (`imageInfo.h:903`) had no action for it → EXIT.
- **Root cause (confirmed):** classifier treated all unformatted writes over GPU-owned targets as `Unsupported`. Streaming-pool reuse produces exactly these writes.
- **Fix (file: `src/graphics/host_gpu/renderer/imageInfo.h`):** restructured the RenderTarget/DepthTarget/StorageTexture cases: formatted writes keep the old rules; **raw** page-aligned writes that contain (or enclose) the image → `Synchronize*` when the target is GPU-owned (`contained && gpu_modified && !buffer_modified`, flush the image to guest memory first so the write lands coherently), else `Invalidate*` (buffer becomes authoritative).
- **Verification (fact):** that EXIT never re-occurred across all later runs. **Unupdated tests (fact):** the `BufferImageWrite` unit cases "target unformatted", "storage unformatted", "depth unformatted" in `tests/ShaderRecompilerComputeTests.cpp` (~lines 13144/13159/13174) still expect `Unsupported` and **will now fail** — the user deferred test updates (see §8).

### 2.6 TextureCache: depth-image readback with buffer-cache overlap (the "readback storage" wall)

- **Symptom (user-reported, then symbolized):** `TextureCache: depth-image readback storage is unsupported, depth=0x20a4e40000+0x20000 linear=0x20000 format=124/124 extent=256x256/256x256 meta=0 buffer=1` (`textureCache.cpp:598` old). Stack: `DispatchDirect → BindDescriptors → FindStorageTexture → DetachStorageDepthAliasLocked → ReadbackWorker::DownloadDepthTarget`.
- **Root cause (confirmed):** `buffer=1` meant the depth image's guest range overlapped buffer-cache pages; the depth readback EXITed on *any* overlap, while the color readback (`DownloadColorImage`, `textureCache.cpp:718-784`) only EXITs on cpu/gpu-modified overlaps and reconciles clean ones via `PublishImageBacking` (which marks the buffer range CPU-modified so buffers re-upload from guest).
- **Fix (file: `textureCache.cpp` `DownloadDepthTarget`):** mirror the color path — compute `buffer_overlap_depth`/`_stencil` and `buffer_cpu/gpu_modified`; EXIT only on `meta_overlap || buffer_cpu_modified || buffer_gpu_modified`; after `WriteBacking`, call `PublishImageBacking` for overlapped depth and stencil ranges.
- **Verification (fact):** never re-occurred.

### 2.7 TextureCache: generalize `SynchronizeDepthImageToBufferLocked`

- **File:** `textureCache.cpp`. Was: full-image writes only, rejected any stencil/htile. Now: contained (partial) writes accepted (containment check mirroring the color helper); the stencil plane is also downloaded + `WriteBacking` + `ForEachDownloadRange` (mirroring `DownloadDepthTarget`'s stencil handling); htile allowed (alignment checks `expected_htile.align==32768`, size match) since htile is derived metadata the guest rebuilds. This is the helper used by every `SynchronizeDepthTarget` action (§2.5, §2.8).
- **Verification:** compiles; no observed in-run invocation produced a new crash. In-run behavior **UNVERIFIED** beyond that.

### 2.8 TextureCache: host write over a GPU-owned depth target

- **Symptom (observed, run coal2):** `TextureCache: host write aliases unsupported image, write=… kind=3 gpu_modified=1 buffer_modified=0 metadata_overlap=0` (`textureCache.cpp:3365` old), from `PrepareHostWrite`.
- **Root cause (confirmed):** `ClassifyHostWriteOverlap` (`imageInfo.h:886`) returns `SynchronizeImage` only when `gpu_synchronizable` — which the caller passed as `kind == RenderTarget`. Depth targets were `Unsupported`.
- **Fix (file: `textureCache.cpp` `PrepareHostWrite`):** `gpu_synchronizable = kind == RenderTarget || kind == DepthTarget`; the synchronize branch routes depth targets to `SynchronizeDepthImageToBufferLocked`, others to `SynchronizeColorImageBackingLocked`.
- **Verification (fact):** never re-occurred.

### 2.9 Memory tracker: nested-fault re-entry (the "upload callback" crash)

- **Symptom (observed, run fix2 + user run):** `memory tracker re-entered from upload callback` (`memoryTracker.h:131` old). With my diagnostic print: `self==owner (same=1), vaddr=0x20a71e0000, thread=GpuWorker state=cp_fault`. Symbolized stack: vectored handler → `GpuResourceManager::HandleFault` → `PageManager::HandleFault` → `FaultThunk` → `BufferCache::InvalidateMemory` → `MemoryTracker::BeginCpuFault` → guard EXIT.
- **Root cause (confirmed):** a page fault on the *same thread* that was inside a `ForEachUploadRange` upload callback re-entered the same `MemoryTracker` instance. The blanket `s_upload_owner != nullptr` guard EXITed for any nesting (it exists to convert a real deadlock — held region spinlocks during `is_written` uploads — into a crash).
- **Fix (files: `memoryTracker.{h,cpp}`, `regionManager.h`):** added `TrackingSpinLock::OwnedByCurrentThread()`. `BeginCpuFault`/`CompleteCpuFault` now, when `s_upload_owner == this`: regions **not** held by this thread are processed normally (their spinlock is free); regions **held by this thread** are *passed through* without touching region state — the outer upload rewrites that state on completion (`ChangeState` + `ApplyGpuProtection`), and the page manager makes the faulted page accessible after completion. The passthrough is recorded in thread_local `s_nested_fault_*` so the matching `CompleteCpuFault` succeeds. All other guard call sites (download/mark/untrack/unmap, which take `m_access_mutex`) keep the fatal EXIT, now with a context print (`KYTY_DIAG tracker re-entry`).
- **Verification (fact):** zero occurrences of that EXIT across all later runs. **Residual risk (theoretical):** for a GPU-dirty page, a nested read can return stale bytes and a nested write skips the download-first ordering — bounded to pages inside a region being uploaded on the same thread; not observed to fire yet.

### 2.10 §3.1 — drain coalescing (shared pause window)

- **Symptom (from doc + re-measured):** thundering herd: bursts like `faults=3175 ... pause=3175(19789.9ms)` and `faults=891 pause=891(1047.3ms)` per wall-second (7–16× wall-clock), ~16 threads in `gpu_pause_lock` at once.
- **Fix (file: `src/graphics/guest_gpu/graphicsRun.cpp`):** new `Gpu::PauseSubmissionsShared`/`ResumeSubmissionsShared` with `SharedPause {mutex, cv, participants, draining}`; `GraphicsRunSubmissionLock` uses them. Leader election is atomic (`participants==0 && !draining` — a mid-drain arriver cannot become a second leader; that race existed in my first draft and was fixed before ever running). Leader parks the worker, `BufferWait()`, `LabelDrain()` once under `m_submission_mutex` (kept only against `Done()`); joiners attach to the open window without that mutex; the last leaver calls `ReleasePause()`. Correctness argument: the worker stays parked for the whole window, so nothing new is submitted and one fence covers all joiners. Old `PauseSubmissions`/`ResumeSubmissions` remain but are now unused.
- **Measured effect (fact):** pause time per wall-second dropped to ~1–3.5 s (e.g. `faults=93 pause=93(3445.4ms)`, `faults=42 pause=42(1180.1ms)`, `faults=118 pause=118(2586.1ms)` vs. 7–16 s before). **Fact:** it did NOT stop the §3.2 abort (fired in the same run) — the abort is not a simple herd-timing artifact.
- **Risk (theoretical):** joiners skip a per-fault `BufferWait`; valid only if no GPU work is submitted while parked, i.e. no submission path bypasses the guest worker. Existence of such a path is **UNVERIFIED**.

### 2.11 Diagnostics: WaitWatch stack capture + new scopes + thread names

- **`src/common/waitWatch.{h,cpp}` (new .cpp):** `DumpThreadStackIfStuck` — for any thread held >3000 ms in a stall dump: `OpenThread`+`SuspendThread`+`GetThreadContext`, then **scans the raw stack** for qwords in the emulator image (`0x140…`) and system DLLs. Prints once per unique (tid, rip). (First version walked rbp — produced garbage like `0xffffffff`/`0x989680` because guest frames are FPO-less; the scan replaced it, confirmed working.)
- **Named threads:** GpuWorker (now carries the real OS tid), `GpuLabelThread`, `TextureReadback` (the texture readback worker). New scopes: `pm_spin`, `pm_handler_inv`, `pm_handler_comp` (pageManager fault phases), `gpu_rbreq`/`gpu_rbcomp` (BufferCache readback), `rb_download` (TextureCache ReadbackWorker::Run), `gpu_tile_lock`/`gpu_detile_lock`, `gpu_tile_fence` (gpuTiler).
- **Result (fact):** these produced the complete deadlock picture in §4 — worker waiting in `ReadbackWorker::Request`; readback worker inside `rb_download` for the same page, both 5–8 s.

### 2.12 Diagnostics: abort/exception visibility

- **`src/libs/libC.cpp`:** abort() diagnostics (registers, pointer/string/byte candidates, stack words with module resolution) converted from LOGF to `std::printf` (visible in Silent). Added: for all-zero buffers, a verdict line `commit-on-fault-filled=YES/no` using new `KernelWasCommittedOnFault` (`src/kernel/memory.{h,cpp}`, records every on-demand committed block in `g_commit_on_fault_blocks`). Added an abort-time dump of the decrypted guest image + `import_symbols->DbgDump("_work","eboot_imports.txt")`.
- **Facts gathered with it:** across 2 abort runs, **every** all-zero buffer verdict was `no` → the doc §3.2(a) "commit-on-fault zero-fill feeds wrong data" hypothesis is **REFUTED**. Abort register state (deterministic across runs): `return=0x0901837ad3`, `rbp=0x07eb…`, `arg0(rdi)=0x090643b4a8`, `arg2=0x11280`, `arg3=arg5=0x4fbd…`; arg0 points to a struct of 3 pointers to zeroed buffers.
- **The abort-time image dump did not produce a file in run fix10 (fact)**; cause UNVERIFIED (branches: program null / base_size 0 / not readable / fopen fail — failure prints were added afterwards and have not been seen in an abort run yet). Made obsolete by §2.13.
- **`src/loader/runtimeLinker.cpp`:** `dump_guest_code` (exception code-bytes dump) converted LOGF→printf. Motivation: the `c000001d` crash — live bytes at the exception address will be printable next occurrence. Not yet observed again.

### 2.13 Method: offline eboot disassembly (no code change)

- **Fact:** the on-disk `eboot.bin` is SELF-wrapped but its ELF payload is **unencrypted** at file offset `0x1a0`. A short Python script parses the ELF program headers and maps image offsets; `_work/eboot_text.bin` is the extracted first LOAD segment (0x465bc0c bytes). Disassemble with `objdump -D -b binary -m i386:x86-64 --adjust-vma=0x0 --start-address=… _work/eboot_text.bin`.
- **Facts established with it:** (a) the abort's `return=0x1837ad3` is **mid-instruction** inside an optimized `memcpy` (`vmovups` copy loops) — not a real return address; the abort's stack is inconsistent (exception/longjmp-shaped). (b) `arg0=0x643b4a8` lies in the eboot's BSS (4th LOAD `filesz=0x24268` < that offset < `memsz`). (c) The calls around the site target PLT stubs (`0x4674eb0` called 10× before the copy, `0x4674ec0` after) — PLT entries `ff 25 jmp *GOT; push idx; jmp resolver`, i.e. imports by index (#0x4d, #~0x50). Import names not yet resolved (see §8).

---

## 3. Challenges faced (what cost time, and what the misleading signals were)

1. **Silent-mode logging (the big one).** `Log::Write` drops everything when `--printf-direction Silent` (`log.cpp:118`), including all the *existing* per-phase recompiler logs. The doc's phase data therefore couldn't exist. Two lessons: (a) every diagnostic that matters must be a plain `std::printf` ("KYTY_DIAG"); (b) `-Printf Console/File` is not a workaround — a File-direction run wrote 38 MB and stalled the game at frame 239 (user then explicitly banned native logs). All new diagnostics were moved to `std::printf`.
2. **A misleading "values stable" measurement.** The provenance hang printed `values=1133` constant — looked like a non-growing oscillation (possibly finite), yet it never terminated. Only after printing the *identity* of the changing entries (`reg31 phi388→phi799→phi657`, periodic with ~393216 visits) did the limit-cycle mechanism become provable. Iteration caps alone would have mislabeled it "slow compile".
3. **The whale vs. the shadow.** The tempting suspect for the §3.2 abort (commit-on-fault zero-fill, doc §4.4 caveat) was cheap to test via block recording + abort verdicts — and was **refuted by data** (all verdicts `no`). Had it not been recorded, more time would've gone into the wrong subsystem.
4. **`Common::Mutex` is not `std::mutex`.** `std::lock_guard`/`std::unique_lock` do not compile on it (`Lock()`/`Unlock()`); use `Common::LockGuard` or manual calls. Cost one failed build.
5. **`windows.h` macro pollution.** Putting `windows.h` in the widely-included `waitWatch.h` turned `Common::File::DeleteFile` into `DeleteFileA` in `emulator.cpp` (macro expansion through the qualified name). Fix: implementation moved to `waitWatch.cpp`. Cost one failed build.
6. **FPO-less stacks.** rbp-chain stack capture returns garbage on guest threads (UE4 release code omits frame pointers). Only raw-stack scans filtered to emulator/system address ranges give usable chains — and they include *stale* stack words, so only the top is trustworthy. All "stack" conclusions here were cross-checked against scopes.
7. **The doc's "deterministic blocker" was only the first wall.** Behind §2 sat a family of texture-cache consistency EXITs (§2.5–2.8), the tracker re-entry (§2.9), the readback deadlock (§4), and the §3.2 abort — each needing its own evidence loop. Expect this pattern to continue; the fixes so far are all of the same shape: the cache/tracker encounters an alias the guest created by pooling memory, and the emulator's conservative EXIT must become a real ownership transition.
8. **Intermittency multiplies run cost.** Each failure mode fires in only some runs (abort ~50%, deadlock ~25%, c000001d once). A single run cannot prove a fix; claims above are based on non-recurrence across all later runs (N≥4 for the texture/tracker EXITs).

---

## 4. What is being fixed right now: the readback deadlock

**State (facts, from stall dumps in runs fix7/coal3–coal9):**
- `GpuWorker state=pm_handler_inv arg0=<page> held 5–8 s, act frozen`. Its captured stack (symbolized): `HostException → GpuResourceManager::HandleFault → PageManager::HandleFault → FaultThunk → TextureCache::InvalidateMemory → MemoryTracker::BeginCpuFault → GraphicsRunFinishScheduler → TextureCache::ReadbackWorker::Request` → blocked in an ntdll wait.
- `TextureReadback state=rb_download arg0=<same page> held 5–8 s`. Its captured stack (symbolized): `Run → DownloadColorImage ("RenderTargetReadback") → Libs::LibKernel::Memory::WriteBacking → DirectMemoryBacking::TryTransferBacking → GpuTile → PageManager::UpdatePageWatchers` (last may be stale scan) → blocked in an ntdll wait.
- Also parked: `RenderThread/PoolThread/RHIThread/tid=0 state=gpu_pause_lock` (joining or leading a pause window), `AgcSubmissionThread state=gpu_idle` (`Done()`), all DRAIN counters 0, `gpu: not blocked on WAIT_REG_MEM`. In one run `GpuLabelThread act` was frozen (640, `state=start`).

**Mechanism (CONFIRMED, session 3 — supersedes the earlier UNVERIFIED guess below).** Ran with the `gpu_tile_*`/`rb_download` scopes + symbolized host stacks (runs `rbd1`/`rbd3`). Both threads stall ~7 s on the **same page** (`0x2071420000` / `0x2070fd0000`):
- `GpuWorker state=pm_handler_inv` (page X). Symbolized stack: `TextureCache::InvalidateMemory → MemoryTracker::BeginCpuFault → GraphicsRunFinishScheduler → FindGpuReadbackPageCandidateLocked → ReadbackWorker::Request → (ntdll wait)`. It is blocked in `ReadbackWorker::Request` waiting for the async readback worker to finish page X, and it **holds `m_resource_mutex`** the whole time (CP-path `FaultScope` in `gpuResourceManager::HandleFault`). (The raw-scan top frame `CommandBuffer::Begin` is a stale return — `context.cpp:238` `Begin()` is a non-blocking `buffer.begin()`.)
- `TextureReadback state=rb_download` (same page X). It is blocked on a Vulkan **fence inside `DownloadColorImage`'s own image→buffer copy** — NOT `g_tiler_mutex` and NOT the tiler fence: the WaitWatch state is `rb_download`, not `gpu_tile_lock`/`gpu_detile_lock`/`gpu_tile_fence`, so the earlier (a)/(b) guess is **refuted**; the symbolized `GpuTile`/`GpuDetile` frames are stale returns from completed tiler calls.
- Single shared Vulkan queue: `graphics.queue` guarded by `graphics.queue_mutex` (`context.cpp:322`); both the guest scheduler and the tiler/readback submit through it.

So the cycle is: **worker (holding `m_resource_mutex`) → waits for the readback worker; readback worker → waits for a GPU fence (its download copy) that never signals.** Why the fence never signals is the remaining sub-question (UNVERIFIED): candidates are (i) the download copy is queued behind an incomplete guest command buffer the parked worker left in-flight on the single queue, or (ii) `queue_mutex`/pool contention. Next probe: a scope around `DownloadColorImage`'s own `WaitForFence` + a `DebugDumpFenceStatus` of the queue at the stall. Fix direction: the CP-thread fault must not block a readback while holding `m_resource_mutex` (the readback worker can need cache state), OR give the readback/tiler an independent queue so it retires while the guest scheduler is stalled.

_Earlier UNVERIFIED note (kept for history):_ the GpuWorker (CP-thread fault) needs a texture readback and waits for the worker's `Ready` (`textureCache.cpp:425-435`). The readback worker is inside `rb_download`. Its wait is one of: **(a)** `g_tiler_mutex`, or **(b)** the tiler's Vulkan fence never signaling. Which of (a)/(b) is UNVERIFIED — now refuted, see above.

**Secondary cycle (confirmed possible by code reading; unconfirmed in the specific dump):** if the readback worker's `WriteBacking`/download triggers a nested page fault, its `GpuResourceManager::HandleFault` takes `GraphicsRunSubmissionLock` (non-CP thread) and can wait on the shared window's `draining` CV — while the window's leader waits for the worker to park, and the worker waits for the readback. §4 fix candidates being weighed: (i) treat readback-worker faults during a CP-prepaused readback as covered (no submission lock, no resource mutex — bare page-manager resolution); (ii) pre-resolve write access for the whole image range on the requesting thread before `Request`. Neither is written yet.

**Also open:** the `c000001d` illegal instruction at `eboot+0x1b0bae5` (file bytes `4c 89 f7` = legal `mov rdi,r14`). Either in-memory text got corrupted or an emulation gap; live-bytes dump (§2.12) will decide on the next occurrence. **UNVERIFIED** whether it recurs.

**The §3.2 abort** is the other open front: deterministic site (mid-`memcpy`), zero-fill refuted, PLT import at #0x4d called 10× before the copy — resolving the import names (via `eboot_imports.txt`, pending an abort run with the current build) is the next evidence step.

---

## 5. Future obstacles and risks

**Measured/observed risks:**
- **Streaming perf is still ~0.2 fps during level load** (DRAIN/s `waitidle≈1.2 s/s`, `pause≈1–3.5 s/s` post-coalescing; frames ~1 per 5 s in watchdog lines). The level has never finished loading in a watched run. Coalescing helped (7–16× → 1–3.5×) but is not sufficient.
- **The whack-a-mole risk is proven, not theoretical:** 3 distinct texture-cache EXITs were hit and fixed this session alone (each time the previous one stopped appearing). More alias shapes likely remain.
- **Test suite now has a known red case:** the 3 `BufferImageWrite` expectations not yet updated for §2.5 (see §8). The pre-existing `SampledColorViews storage` death-case failure (`SelectStorageColorView` fatal guard not firing, child exit 127≠321) exists **independently of this session's changes** (verified by diff: `imageView.h` untouched).

**Correctness risks of this session's changes (theoretical unless noted):**
- `ClassifyBufferImageWrite` (§2.5): a truly-pathological alias that *should* have crashed now silently synchronizes or invalidates — misrender instead of EXIT. Deliberate trade, mirrors §4.3's philosophy.
- Nested-fault passthrough (§2.9): bounded stale-read/lost-write leak on GPU-dirty pages inside a region mid-upload on the same thread. Not observed firing.
- Shared pause (§2.10): relies on "worker parked ⇒ no submissions". A submission path bypassing the worker would break the shared fence. Existence UNVERIFIED.
- `SynchronizeDepthImageToBufferLocked` (§2.7): stencil now flushed alongside depth; htile ignored. `expected_*` layout checks can still EXIT on unusual depth formats (d16/d32 only, 64 KB alignment, layers==1) — a differently-shaped depth target would hit that EXIT.
- Phi-slot fix (§2.1): single-arg phis are semantically the value (`ValueResolved` walks `phi_args`), so consumers are equivalent; precision loss in degenerate CFGs is possible but benign. Suite passes.
- `g_commit_on_fault_blocks` (§2.12) grows unboundedly during a run (vector per commit). Diagnostic; must be stripped or capped before release.

**Deferred redesign (fact):** the fault/lock architecture keeps producing new deadlock shapes (§4 is the third distinct cycle: label-callback §4.5, tracker re-entry §2.9, readback §4). The code itself notes it (`gpuResourceManager.cpp:74-77`: "A real fix must drain only for sync-relevant faults, which is a fault/lock redesign"). Each added exemption narrows the invariant surface; a redesign is the real answer but is out of scope until the level loads.

---

## 6. Build, run, reproduce

```
build.bat prod            # exe: _Build\windows-prod\install\kyty_emulator.exe
build.bat test            # builds + runs all test exes in _Build\windows-nolauncher
```

Headless driver (project root; injects input via Win32 PostMessage; window title carries `frame: N, fps: F`):
```
wsl --shutdown            # free system commit before each launch
.\drive_gta3.ps1 -IntroSeconds 40 -PostStartSeconds 60 -LogTag TAG [-Printf Silent]
# env for shader dumps:  $env:KYTY_DUMP_PRECMP=1  (bash: KYTY_DUMP_PRECMP=1)
```
- Logs: `_gta3_logs\gta3_TAG.out.log` (+`.err.log`). Keep `--printf-direction Silent` (native logs spam + change timing; guest `LOGF` is suppressed anyway). Emulator `LOGF` is ALSO dropped in Silent (`log.cpp:118`) — only `std::printf` (KYTY_DIAG) and fatal prints appear.
- Grep keys: `DRAIN/s`, `KYTY_DIAG`, `WAITWATCH (stall`, `host stack`, `name=GpuWorker`, `--- Error ---`, `Num compiled`, `Guest abort`, `state=(pm_|gpu_rb|rb_download|gpu_tile|cp_fault|gpu_pause_lock|gpu_idle|shader_ps)`.
- Exit codes: `321` = clean `EXIT()`/DbgExit; `-1073741787` = `0xC0000005`; `127` (test child) = death-case guard did not fire; `c000001d` = illegal instruction.
- Offline shader repro: `_Build/windows-prod/shader_recompile_file.exe _work/hang_ps.bin ps 60` (or `... ps dumpcfg`). Input dumps come from `_Shaders/precmp/` (with `KYTY_DUMP_PRECMP=1`); the hanging one is the last file written.
- Stack symbolization: `llvm-symbolizer --obj=_Build/windows-prod/install/kyty_emulator.exe 0x140...` (PDB: `_Build/windows-prod/kyty_emulator.pdb`).
- Offline eboot disassembly: SELF payload is plain ELF at eboot.bin offset `0x1a0`; extracted text at `_work/eboot_text.bin`; `objdump -D -b binary -m i386:x86-64 --adjust-vma=0x0 --start-address=0x… --stop-address=0x… _work/eboot_text.bin`.

---

## 7. Instrumentation / scaffolding added (strip or gate before release)

| What | Where | Notes / cost |
|---|---|---|
| `KYTY_DUMP_PRECMP` shader dump | `shader.cpp` `DumpShaderRecompilerPrecmp` | env-gated; file I/O per shader when on |
| `shader_recompile_file` harness | `tests/ShaderRecompileFile.cpp`, CMake | EXCLUDE_FROM_ALL; harmless to keep |
| Phase LOGF lines | `ShaderRecompiler.cpp` (6 lines) | sprintf cost per shader even in Silent; matches existing style |
| WaitWatch scopes (this session) | `pm_spin`, `pm_handler_inv/comp` (pageManager.cpp), `gpu_rbreq/gpu_rbcomp` (bufferCache.cpp), `rb_download` (textureCache.cpp), `gpu_tile_lock/detile_lock`, `gpu_tile_fence` (gpuTiler.cpp) | small; 2 on hot paths (`gpu_tile_*`, `pm_*`) |
| Host stack capture | `waitWatch.cpp` `DumpThreadStackIfStuck` | suspends stuck threads; perturbing; strip for release |
| Thread names | GpuWorker real tid, `GpuLabelThread`, `TextureReadback` | trivial; can stay |
| `KYTY_DIAG` printfs | commit-on-fault (memory.cpp), tracker re-entry (memoryTracker.h), abort-diag (libC.cpp), exception code dump (runtimeLinker.cpp) | abort/exception paths only except commit-on-fault (powers of 2) |
| `g_commit_on_fault_blocks` | memory.cpp | **grows unboundedly per run** — strip or cap |
| Abort-time image/import dump | libC.cpp | abort path only; image dump never fired successfully (UNVERIFIED why) |
| Prior-session scaffolding (doc §5) | DRAIN/s line, other scopes, `SetThreadName` | strip with the above |

**Keep (real changes, not scaffolding):** §2.1 phi fix + 16M iteration cap, §2.5–2.8 texture-cache support, §2.9 nested-fault passthrough, §2.10 shared pause, the `word_count==0` decoder guard (prior session).

---

## 8. Concrete next steps (prioritized)

1. **Close the readback deadlock (§4).** Run with the `gpu_tile_lock`/`gpu_tile_fence` scopes: identify whether the readback worker blocks on `g_tiler_mutex` (someone wedged holding it) or on the tiler fence (queue can't retire the detile job). Then fix the cycle at its root — candidates: make readback-worker nested faults during a CP-prepaused readback bypass the submission lock/resource mutex (bare page-manager resolution), or pre-resolve write access for the whole image range on the requesting thread before `ReadbackWorker::Request`. Proof = a stall-free run through the current ~17 s wall, twice.
2. **Name the abort's subsystem (§3.2).** Get an abort run with the current build: `_work/eboot_imports.txt` (import map) + the PLT indices around the site (#0x4d, #~0x50) should yield the import names called by the crashing `memcpy` function. If the image-dump failure prints fire, diagnose them too. Also re-check the live bytes at the abort/exception addresses for text corruption (ties to the `c000001d` case: if live bytes ≠ file bytes at `0x1b0bae5`, hunt the misdirected write — prime suspect class: a cache `WriteBacking` to a stale image range).
3. **Update the 3 `BufferImageWrite` test expectations** (deferred by user mid-session): "target unformatted" → `SynchronizeRenderTarget`, "storage unformatted" → `SynchronizeStorageTexture`, "depth unformatted" → `SynchronizeDepthTarget`; add partial-raw cases. Re-run `build.bat test`; confirm the only remaining red is the pre-existing `SampledColorViews storage` case.
4. **Perf re-measurement after stability:** DRAIN/s pause/waitidle targets (<1 s/s), shader-compile cost (the §2 shader emits 205184 SPIR-V words via the dispatcher fallback — driver-side compile time may be significant; the persistent `_pipeline_cache.bin` mitigates across runs).
5. **Commit the session's work** (nothing is committed yet), then **strip §7 instrumentation** before any "final" fps measurement.
6. **Later, not now:** the fault/lock redesign noted in `gpuResourceManager.cpp:74-77` (drain only for sync-relevant faults); evaluating a fallback/skip path in `RefreshShaders` for future non-converging shaders (the 16M cap turns them into hard EXITs today).
