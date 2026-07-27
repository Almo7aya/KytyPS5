# GTA III: The Definitive Edition (PPSA03527) — Issue Log & Handoff

**Emulator:** KytyPS5 (Vulkan backend, GCN→SPIR-V shader recompiler)
**Branch:** `fix/gta3-menu-start-crash`
**Game path:** `Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin`
**Engine:** UE4 + RAGE (thread names TaskGraphThreadNP/HP/BP, RHIThread, RenderThread, AgcSubmissionThread, PoolThread, `[RAGE] netThrPool`).

This document is **evidence-based**. Each item states the **observed symptom** (log lines / measurements), the **confirmed root cause** (confirmed = a fix changed the observed behavior, or a thread dump directly showed it), the **fix** (if made), and **open questions** where the mechanism is not yet proven. Anything not directly observed is labeled `UNVERIFIED`.

---

## 0. Current status (fact)

- Before this work: the game **crashed at Start** (menu → click Start → immediate crash, `0xC0000005` access violation).
- Now: the game **loads the menu at ~9–11 fps, accepts Start, compiles ~497 shaders, and runs deep into level streaming (~14 s past Start)**, then **deterministically freezes** in the pixel-shader recompiler (see §2). ~40% of runs instead hit a timing-dependent `Guest abort()` (see §3.2).
- A level has **not** yet fully loaded into gameplay. The single deterministic blocker is §2.

Commits on the branch (all `--no-gpg-sign` because the headless environment has no gpg passphrase):
- Prior session: `5954f3b`, `ad90964`, `762305c`, `776ba29`, `cb47175` (keyboard input, cube-RT readback, label-fault guard, texture alias, host-write, watchdog, pipeline cache).
- This session: `a700364` (menu-thrash fix + unbounded buffer clamp + instrumentation), `041788e` (label-deadlock fix + commit-on-fault + storage-RT retire), `25521a5` (shader-hang localization).

---

## 1. Build & reproduce (fact)

```
build.bat prod
# exe: _Build\windows-prod\install\kyty_emulator.exe
```

Headless driver (in the project root, injects input via Win32 PostMessage; SDL translates WM_KEYDOWN):
```
.\drive_gta3.ps1 -IntroSeconds 40 -PostStartSeconds N -LogTag TAG [-Printf Console]
```
- Located at the **project root** (`drive_gta3.ps1`). It resolves the emulator exe relative to its own location (`$PSScriptRoot`) and writes logs to `_gta3_logs\` next to itself.
- Presses `j` (Cross, scancode 0x24) every 5 s to skip intros, then Enter (Options, 0x1C) = Start.
- Default `--printf-direction Silent` (crashes + `KYTY_DIAG` printfs still print; guest `LOGF` is suppressed). `-Printf Console` prints everything (huge, and slows the game enough to change race timing).
- Log at `_gta3_logs\gta3_TAG.out.log` (stderr: `.err.log`). Grep: `DRAIN/s`, `KYTY_DIAG`, `WAITWATCH (stall`, `name=GpuWorker`, `--- Error ---`, `Num compiled`.
- The game path is hardcoded in the script (`$game`); edit it if the game lives elsewhere.
- Run `wsl --shutdown` before each launch to free system commit for the memory SelfTest.
- Window title carries live `frame: N, fps: F` — the driver logs it each tick. **`frame` is the presented-frame counter; it freezes when the loading screen stops presenting even though background work continues.**

Crash exit codes: `321` = clean `EXIT()`/DbgExit; `-1073741787` = `0xC0000005` access violation.

---

## 2. **CURRENT BLOCKER — pixel-shader recompiler infinite loop** (fact-localized; exact loop UNVERIFIED)

### Observed
- With all §4 fixes in, the game reaches deep level streaming, then the single GPU worker thread (`tid=-2 name=GpuWorker`) **freezes for the entire watch window (confirmed frozen 240 s in one run)**. `frame` freezes (seen at 220, 223, 263, 264, 265 across runs — the exact point varies with timing). `fps` ≈ 0.14. All `DRAIN/s` counters go to 0 (no faults, no submits).
- The `Num compiled N shaders` counter climbs to **497** and then **stops** (does not advance during the freeze).
- WaitWatch stall dumps show the worker deterministically in:
  `state=shader_ps arg0=0x206bdf0000` with `act` (activity counter) **frozen** and `held_ms` climbing (2470→4471→5325…).
- Call path (localized with nested WaitWatch scopes): PM4 opcode `0x35` = `IT_DRAW_INDEX_OFFSET_2` → `CommandProcessor::DrawIndexOffset` (`graphicsRun.cpp:1012`) → `RenderDrawIndex` (`renderDraw.cpp:953`) → `RefreshShaders` (`renderDraw.cpp:678`) → **`ShaderCompileInfoPS`** for the **pixel shader at guest address `0x206bdf0000`**.

### Confirmed facts about the loop
- It is a **true infinite loop / non-terminating pass, not slow compilation**: `act` frozen (no WaitWatch sub-events), shader count stuck, frozen ≥240 s.
- It is **post-decode**. A `word_count==0` guard was added to the GCN decode loop (`ShaderDecoder.cpp` ~line 386, the `for (uint32_t word_index = 0; word_index < code.size();)` loop whose only advance is `word_index += inst.word_count`). **That guard never fires**, so the decode loop is not the hang.
- **Ruled out** (each has a WaitWatch scope that did NOT show at the stall): GPU readback wait (`FinishReadbackTransaction`/`gpu_readback`), the render-context mutex (`render_ctx_lock`), CP-path page faults (`cp_fault`), commit-on-fault (`commit_fault`), `BufferWait` (`gpu_bufwait`), `BufferFlush` (`gpu_flush`), and the CE/DE for-loop spin (a spin counter never logged).
- The CFG irreducibility detector (`ShaderCFG.cpp` `ComputeComponents`, ~line 780) **detects irreducible control flow and bails gracefully** (sets `FailureKind::IrreducibleControlFlow`), so it is not an unbounded loop.
- `ComputeReachableBlocks` (`spirvEmitterControlFlow.cpp:5`) has a visited check → terminates.

### Not yet determined (UNVERIFIED)
- The exact non-terminating loop. Remaining candidates (post-decode, none yet instrumented): IR construction (`shaderIR/ShaderIR.cpp`), structured-control-flow recovery / SPIR-V emission (`SpirvEmitter.cpp`, `spirvEmitter/*.cpp`), or a fixpoint loop in `ShaderCFG.cpp` (`while (changed)` at lines ~556 and ~590).

### Suggested next actions
1. Dump the GCN bytecode at guest addr `0x206bdf0000` (size from the PS stage registers) to a file for offline analysis, and/or feed it through the recompiler in a standalone test (`shader_recompiler_*_tests`).
2. Add iteration caps + a `KYTY_DIAG` printf to each remaining post-decode loop to identify the non-terminating one, **or** capture a host stack of the frozen `GpuWorker` (the crash handler already has `SysStackWalkX86` in `runtimeLinker.cpp:482`; the OS thread id is not currently plumbed into the worker's WaitWatch entry — `SetThreadName("GpuWorker", -2, 0)` sets host_tid=0).
3. Fix the identified loop. **Or** add a fallback path: on shader-compile timeout, return a fallback PS or make `RenderDrawIndex` skip the draw instead of `EXIT`-ing (`RefreshShaders` currently `EXIT`s on PS compile failure — no fallback exists; `GetNextGenFallbackShaderId` in `shader.cpp:437` is a starting point). Skipping would let the level load with that object mis-rendered.
- Note: there is **no on-disk cache for recompiled (GCN→SPIR-V) shaders** — only the in-memory `g_shader_program_cache` and the persisted **Vulkan** `VkPipelineCache` (`_pipeline_cache.bin`). So this shader is recompiled every run and the hang reproduces 100%.

---

## 3. Other open issues observed this session (not yet fixed)

### 3.1 Thundering-herd serialization on the fault drain (fact; perf)
- During level streaming, WaitWatch dumps show **~16 engine threads simultaneously in `state=gpu_pause_lock`** (blocked entering `Gpu::PauseSubmissions`, i.e. contending the single `m_submission_mutex`).
- `DRAIN/s` measured bursts like `faults=5641 ... pause=5641(14987.1ms)` and `faults=1519 pause=1519(7325.9ms) waitidle=7156.2ms` in a single wall-second — i.e. total time-in-drain summed across threads is 7–16× the wall-clock, confirming heavy serialization.
- A coalescing (shared pause window, ref-counted) variant was implemented and then **removed** this session because it was suspected (incorrectly) of causing the level-load hang; the real hang was the label callback (§4.5). Coalescing is a **valid future perf change** but was not the blocker.
- **Status:** unfixed. The drain is currently serialized per-fault via `m_submission_mutex`.

### 3.2 Timing-dependent `Guest abort()` (~40% of runs) (fact; root cause UNVERIFIED)
- ~40% of level-load runs terminate at ~10–14 s with `EXIT("Guest abort()\n")` at `libC.cpp:253` (the guest's own `abort()`), called from guest code (stack frame `[2] 0x0901837ad3`). Exit code `321`.
- It is **timing-dependent**: a `-Printf Console` run (much slower, less concurrency pressure) did **not** abort; it hung in the recompiler (§2) instead. So it is a race, not deterministic.
- `KYTY_DIAG` showed no unresolved AV before these aborts (the AV path is handled), so the guest reached its own consistency check and failed it.
- **UNVERIFIED hypotheses** (do not treat as fact): could be (a) the commit-on-fault zero-fill (§4.4) feeding wrong data where the guest expected content, or (b) a concurrency data race under the herd (§3.1). Not proven either way.
- **Status:** unfixed / not root-caused.

### 3.3 Level-load is slow even without crash/hang (fact; partly explained)
- When it neither crashes nor (yet) hangs, `frame`/fps sit near 0.14 during streaming. Part of this is inherent per-frame `Done()` full GPU drain; part is the shader-compilation cost (497 shaders, some slow).
- **Status:** the "reasonable framerate" goal is not met even before the §2 hang; needs the drain/async work plus shader-compile improvement.

---

## 4. Fixed this session (fact — confirmed by observed behavior change)

### 4.1 Menu-load synchronous-GPU-drain thrash — FIXED (`a700364`)
- **Symptom:** menu/loading ran at ~0.15 fps.
- **Measurement (fact):** instrumented every drain path (`DrainStats` in `common/waitWatch.h`, printed per second as `=== DRAIN/s ... ===` by the watchdog in `emulator.cpp`). During menu load: **`waitidle` (`Gpu::WaitForIdle`) ≈ 940 ms of every 1000 ms**; `bufwait` (the actual GPU fence, `CommandProcessor::BufferWait`) ≈ 40 ms/s; `labeldrain` ≈ 0.2 ms/s; `incDe` = 0. So the per-fault drain's cost was almost entirely the **full guest-submission-queue drain**, not the load-bearing GPU fence/label flush.
- **Fix:** in `graphicsRun.cpp`, the per-fault drain (`Gpu::PauseSubmissions`) now uses a lightweight **worker park** (`RequestPauseAndWait`: set `m_pause_requested`, wait for the worker to finish its in-flight `Process` and set `m_worker_parked`) instead of `WaitForIdle` (which drained the whole queue). It still does `m_gfx_cp->BufferWait()` + `LabelDrain()`. `Gpu::Done()` (frame flip) keeps the full `WaitLocked()` drain. Mutual exclusion between faults and `Done()` is the original `m_submission_mutex`.
- **Verification (fact):** menu now reaches frame ~255 at ~9–11 fps in ~36 s (was ~230 at ~8.8 fps and thrashing).

### 4.2 Unbounded storage-buffer descriptor crash — FIXED (`a700364`)
- **Symptom:** right after Start, `EXIT("BufferCache: GPU-read access denied, addr=0x1141000e00 size=0xffffffff")` at `bufferCache.cpp:1263`.
- **Root cause (confirmed):** a storage-buffer descriptor with `num_records ≈ 0xffffffff` (a 4 GB footprint) whose tail pages are not resident; `ValidateGpuAccess` requires the whole range GPU-mapped.
- **Fix:** added `PageManager::GpuAccessExtent(vaddr, max_size, access)` (`pageManager.cpp`) and `BufferCache::GpuAccessExtent` (`bufferCache.cpp`); `NativeStorageBuffer` in `descriptors.cpp` clamps the bound range to the contiguous resident extent (binds null if nothing resident). Robust buffer access zero-fills reads past the clamp.
- **Verification:** that abort no longer occurs; the game proceeds past it.

### 4.3 Storage→render-target alias crash — FIXED (`041788e`)
- **Symptom:** during menu/intro, `EXIT("TextureCache: unsupported render-target alias, requested=0x200f540000+0x380000 existing_kind=1 existing=0x200f540000+0x400000 ... gpu_modified=0 ...")` at `textureCache.cpp:2114`. `existing_kind=1` = `CachedImage::Kind::StorageTexture`.
- **Root cause (confirmed):** a **clean** StorageTexture (`gpu_modified=0`) overlapped by a new render target; `ClassifyStorageRenderTargetOverlap` (`imageInfo.h`) returned an overlap type only valid for RenderTarget/DepthTarget kinds, so `supported=false` → abort. `HasGuestCurrentImageOwnership` required all of `!gpu_modified && !buffer_modified && !cpu_dirty && !tracker_gpu_modified`, which this case did not satisfy.
- **Fix:** in `ClassifyStorageRenderTargetOverlap`, added: if `!storage_gpu_modified` (the storage image is not its own authoritative GPU copy), return `RetireStorage` — retiring loses nothing because guest memory or a buffer is current.
- **Verification:** that abort no longer occurs across subsequent runs.

### 4.4 Reserved-memory access violation — FIXED via on-demand commit (`041788e`)
- **Symptom:** ~10–14 s after Start, `Access violation: Write/Read [addr]` reaching the fatal handler at `runtimeLinker.cpp:743`. Addresses observed: `0x20acc12d40`, `0x211d55ac00`, `0x211df1ac00`, `0x2126b4ac00` (write), `0x50aa0008`, `0x50bd0008` (read).
- **Facts gathered** (via a `KYTY_DIAG` probe in the AV handler):
  - The GPU-aperture addresses (`0x20…`/`0x21…`) had OS `state=0x2000` (MEM_RESERVE, uncommitted), `protect=0`, inside a single ~450 GB anonymous reserved VirtualRanges entry (e.g. `kstart=0x20a7b10000 kend=0x8fc0000000 kname=anon`).
  - Map/unmap logging showed **8969 `KernelMapDirectMemory` maps and 0 unmaps** over a run; the faulting 64 KB blocks were **never mapped** (VirtualQuery showed a large uncommitted reserve starting exactly at the block). The AV write addresses were strided at the **same 64 KB-block offset `0xac00`** — i.e. the guest iterates a pool of 64 KB blocks and touches ones it never explicitly mapped.
  - The low read addresses (`0x50aa0008`, `0x50bd0008`) were `MEM_RESERVE` but **not tracked in `g_virtual_ranges`** (`KernelVirtualQuery` failed, `kname` empty).
  - Confirmed pre-existing: the stable (pre-change) build also crashed at Start with `0xC0000005`.
- **Fix:** `KernelCommitReservedOnFault(vaddr)` in `memory.cpp` (declared in `memory.h`), wired into the AV handler in `runtimeLinker.cpp` for **both read and write** faults. It commits the 64 KB block containing the fault: first the tracked path (`g_virtual_ranges->ConsumeReserved` + `g_placeholder_address_space->Commit` / `CommitFixedHostRange`, then re-register as committed Flexible), then an OS-level fallback (`VirtualQuery` → if `MEM_RESERVE`, `Commit`/`CommitFixedHostRange`) for untracked pages. Zero-fill matches Windows fresh-commit semantics.
- **Verification:** the AV no longer terminates the run; a 60–120 s watch survives without that crash; `KYTY_DIAG commit-on-fault #N` counters climb (e.g. to 256+).
- **Caveat (UNVERIFIED):** the zero-fill may be wrong if any of these blocks are supposed to alias direct-memory content the guest wrote via a different VA; this is a candidate cause of §3.2's abort but is not proven.

### 4.5 Label-callback ↔ Done ↔ worker deadlock — FIXED (`041788e`)
- **Symptom:** the game hung (worker frozen, `frame` frozen ~240, everything idle).
- **Root cause (confirmed by thread dump):** the WaitWatch stall dump showed a 3-way cycle:
  - `AgcSubmissionThread` in `Gpu::Done()` → `WaitLocked` → `WaitForIdle` (`state=gpu_idle`, `m_submission_count` non-zero) holding `m_submission_mutex`, waiting for the worker to drain.
  - `GpuWorker` in a PM4 handler (`state=gpu_pm4`) waiting for a GPU label/EOP value.
  - The GPU **label-callback thread** (unnamed, `tid=0`) in `state=gpu_pause_lock` — i.e. it faulted on GPU-tracked memory inside a callback and blocked on `m_submission_mutex` (held by `Done()`), so it could never write the value the worker was waiting for. → Done → worker → callback → Done.
- **Fix:** in `GpuResourceManager::HandleFault` (`gpuResourceManager.cpp`), when `LabelInCallback()` is true, resolve the fault under `m_resource_mutex` alone (no `GraphicsRunSubmissionLock` / GPU pause). Justification (confirmed consistent): a label callback only writes sync values for pipeline work the GPU has already retired (end-of-pipe), so the region is no longer in flight.
- **Verification:** after this fix the worker progresses past this point (the label thread `tid=0` is `state=start`/idle at later stalls, not blocked), reaching the §2 shader hang.

### 4.6 Prior-session fixes still in place (fact; committed earlier)
From `cb47175` and the prior-session commits (documented in project memory `gta3-definitive-launch`):
- `controller.cpp`: keyboard (`KEYBOARD_CONTROLLER_ID`) merges into the active pad even when a real gamepad holds the active slot (enables headless input).
- `textureCache.cpp DownloadColorImage`: layered (cube, layers>1) render-target mip-chain readback support.
- `imageInfo.h ClassifyBufferImageWrite`: Texture case also invalidates when `image_contained`.
- `imageInfo.h IsHostWriteRefreshable`: StorageTexture returns `!buffer_modified`.
- `gpuResourceManager.cpp`: removed a `LabelInCallback()` abort (label callbacks routed through the normal fault path).
- `776ba29`: persistent `VkPipelineCache` (`_pipeline_cache.bin`), seeded at device init, saved every ~15 s by the watchdog. **Note (fact):** no measurable time-to-menu improvement for GTA III — startup is not driver-pipeline-compile-bound.

---

## 5. Diagnostic scaffolding currently in the tree (fact — REMOVE before release)

These were added for this investigation and add overhead (some on hot paths); strip before shipping / before measuring final fps:
- `common/waitWatch.h`: `DrainStats` namespace (atomic counters + `ScopedNs`), `NowNs()`.
- `emulator.cpp` `WatchdogRun`: the per-second `=== DRAIN/s ... ===` line (frame, faults, cpFaults, submits, pause/waitidle/bufwait/labeldrain ns, incDe).
- WaitWatch `Scope`s (hot-path ones marked): `gpu_process`, `gpu_pm4`, `gpu_begin`, `gpu_flush`, `gpu_bufwait` (hot), `gpu_readback`, `cp_fault`, `commit_fault`, `render_ctx_lock`, `draw_prepare`, `draw_shaders`, `shader_vs`, `shader_ps`; plus the current-PM4-opcode tag written into `Self().arg0` per packet (hot, `graphicsRun.cpp`), the shader guest-address tags, and `SetThreadName("GpuWorker", ...)`.
- `KYTY_DIAG` printfs: commit-on-fault counters (`memory.cpp`), and (already removed) the AV-characterization probe in `runtimeLinker.cpp`.
- `ShaderDecoder.cpp`: the `word_count==0` guard is a **real safety fix** (keep it), though it does not fire for the §2 shader.
- `drive_gta3.ps1` (project root): the headless test driver (added a `-Printf` parameter; writes logs to `_gta3_logs\`). Keep for testing; not part of the emulator build.

---

## 6. Priority order for the next agent (recommendation)

1. **§2 — the PS recompiler infinite loop on shader `0x206bdf0000`.** This is the deterministic blocker to loading a level. Dump the shader, find the non-terminating post-decode loop, fix it (or add a fallback/skip). Highest priority.
2. **§3.2 — the `Guest abort()` race** (only appears ~40% of runs; may or may not disappear once §2/§4.4 are fully correct).
3. **§3.1 / §3.3 — level-load performance** (thundering-herd coalescing; shader-compile cost; async submission) — needed for the "reasonable framerate" goal after the level loads.
4. Strip §5 instrumentation and re-measure.
