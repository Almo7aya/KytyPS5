# GTA III DE (PPSA03527) — Continuation Plan & Current Findings (state capture)

**Purpose:** resume work after a context compaction with zero re-investigation. Everything here is verified against logs/diffs as of this writing. Mission: get the game loading a level at good fps on branch `fix/gta3-menu-start-crash`. Deep history: `docs/gta3-definitive-issues.md` (session 1), `docs/gta3-worklog.md` (session 2, kept current by commits below).

---

## 1. Repo state right now

**Commits (top = newest):**
- `56358fb` diag: bounded `WaitForFenceOnly` probe; fence-stall did NOT fire → GPU-fence theory refuted.
- `fe35109` diag: readback-deadlock mechanism confirmed (worklog §4).
- `4857651` wip: session-2 fixes (recompiler hang, texture-cache aliases, tracker re-entry, drain coalesce).
- `25521a5` base.

**Uncommitted in working tree (DO NOT revert):**
1. User's fix attempts (keep, unproven): `GraphicsRunAdoptedSubmissionPause` (graphicsRun.{h,cpp}) + readback-worker adoption (`textureCache.cpp` `ReadbackWorker::Request/Run`, `command_request` flag); pageManager read-fault-on-write-watched-page handling (returns `allowed` instead of FailFast); waitWatch.cpp capture dedup key `(tid, rip, held_ms/4000)`.
2. **My pinpoint scopes, JUST ADDED and UNBUILT** (finish before building):
   - `dmem_lock` in `TryTransferBacking` (`src/kernel/memoryAddressSpace.inc`) + `#include "common/waitWatch.h"` added to `src/kernel/memory.cpp` (line ~8) — DONE.
   - `mt_dl` in `MemoryTracker::ForEachDownloadRange`, `mt_ul` in `ForEachUploadRange` (`src/graphics/host_gpu/memoryTracker.h`) — DONE.
   - `bc_publish` in `BufferCache::PublishImageBacking` (`bufferCache.cpp`) — DONE.
   - **`xfer_qidle` in `Transfer::WaitForQueueIdle` (`src/graphics/host_gpu/transfer.cpp:85-90`) — EDIT FAILED 2×, MUST REDO.** File is LF-only (the Read tool's `\r` markers were misleading; bytes verified with `od`). Exact target text (tab indent, spacing verified):
     ```
     	Common::LockGuard lock(graphics.queue_mutex);
     	const auto        result = graphics.queue.waitIdle();
     ```
     Insert `Kyty::WaitWatch::Scope w("xfer_qidle", 0, 0); // KYTY_DIAG` between them. transfer.cpp already has waitWatch.h? — CHECK; if missing add `#include "common/waitWatch.h"`.
   - **`rb_wait` scope still TO ADD:** `ReadbackWorker::Request` wait loop, `textureCache.cpp` (~line 425-435, the `while (true) { ... state.wait(...) }` after `state.store(State::Requested)`).
3. Untracked helpers: `tests/ShaderRecompileFile.cpp`, `_work/` (eboot slices, `hang_ps.bin`, `cfg_dump.txt`, `eboot_imports.txt`), `_gta3_logs/`, `drive_gta3.ps1`.

---

## 2. The current investigation (everything known about the readback deadlock)

**Symptom (reproduced many runs):** mid level-load, all DRAIN counters go 0, frame freezes. WAITWATCH shows:
- `GpuWorker state=pm_handler_inv arg0=<page>` held 5–8 s — stuck in `TextureCache::InvalidateMemory` → CP-path → `ReadbackWorker::Request` wait for `State::Ready`, **holding `m_resource_mutex` (FaultScope) + `m_fault_mutex`**.
- `TextureReadback state=rb_download arg0=<same page>` held 5–8 s — stuck inside the async readback worker's `Run()`.
- Others: `AgcSubmissionThread state=gpu_idle` (Done), `RenderThread/PoolThread/tid=0 state=gpu_pause_lock`, guests in `cond`.
- `gpu: not blocked on WAIT_REG_MEM`.

**Confirmed refutations (do not re-test):**
- NOT `g_tiler_mutex` (state would be `gpu_tile_lock`/`gpu_detile_lock`).
- NOT a `CommandBuffer` GPU fence (`fence-stall` probe in `context.cpp` never fired, run `fst2`).
- NOT `gpu_pause_lock`/`PauseSubmissionsShared` (state would show `gpu_pause_lock`; it shows `rb_download` — this is why the adopted-pause fix may not remove the observed block).
- `cache.m_lock` spinlock would burn CPU, not park (rip is an ntdll OS wait).
- Symbolized readback stack (raw scan, stale words possible): `Run → DownloadColorImage → WriteBacking → DirectMemoryBacking::TryTransferBacking → GpuTile / BufferCache::UnmapMemory / PageManager::UpdatePageWatchers / MemoryTracker::Iterate`.

**The open question:** which UNSCOPED OS wait is the readback worker parked on? Candidates, ranked:
1. `DirectMemoryBacking::m_mutex` (`TryTransferBacking`, std::mutex) — held by any thread doing backing transfers (map/unmap paths, other `WriteBacking` callers).
2. texture memory tracker's `m_access_mutex` (`ForEachDownloadRange`/`ForEachUploadRange`, std::mutex) — `ForEachUploadRange` holds it across the ENTIRE upload incl. Vulkan calls; a wedged uploader blocks all downloaders.
3. `BufferCache::PublishImageBacking` → `FaultSafeCacheLock(m_mutex)`.
4. `graphics.queue_mutex` (`Transfer::WaitForQueueIdle`, GpuTile submit path).

**The probe (this is the immediate task):** the scopes above name BOTH the waiter AND the holder — a stall dump will show the holder thread sitting in `mt_ul`/`dmem_lock`/`bc_publish`/`xfer_qidle` with held_ms climbing.

**Cycle shape (confirmed parts):** worker (holds resource/fault mutexes) → waits for readback Ready; readback worker → blocked on <primitive>; whoever holds <primitive> → (probably) blocked on the pause window or on the worker. Fix directions already sketched in worklog §4: (a) do the texture readback INLINE on the CP thread (precedent: the buffer readback already works inline via `GraphicsRunFinishScheduler`); (b) fix the specific lock ordering; (c) the adopted-pause in the tree covers the `UnmapMemory→GraphicsRunSubmissionLock` sub-case (keep, but evidence says the observed block is elsewhere).

---

## 3. Other open fronts (evidence preserved)

**§3.2 `Guest abort()` (~50% of runs, timing-dependent, deterministic site):**
- Caller `eboot+0x1837ad3` = **mid-instruction inside an optimized `memcpy`** (offline disasm of `_work/eboot_text.bin`; the SELF payload is unencrypted ELF at eboot.bin offset `0x1a0`). Registers: `arg0(rdi)=eboot+0x643b4a8` (BSS), `arg2=0x11280`, `arg3=arg5=0x4fbd…` heap. arg0 → struct of 3 pointers to zeroed buffers.
- **Zero-fill hypothesis REFUTED:** all all-zero buffers verdicted `commit-on-fault-filled=no` (two abort runs).
- The `memcpy` function calls PLT import **#0x4d** 10× before the copy and **#~0x50** after. `_work/eboot_imports.txt` (1790 lines) maps imports as NID strings (`bzQExy189ZI[libc_v1][libc_v1.1][Func]`), `vaddr=0`. **To name #0x4d: line order likely == PLT order (UNVERIFIED) → check lines 78(0x4d+1) and 81(0x50+1); NID→name via the emulator's registered NIDs in `src/libs/`.**
- Abort-time diagnostics are live (`KYTY_DIAG abort-diag` in `libC.cpp`, prints registers/stack-words/module offsets in Silent mode).

**`c000001d` illegal instruction (once, run coal8):** at `eboot+0x1b0bae5`; file bytes there = `4c 89 f7` (legal `mov rdi,r14`) → in-memory text corrupted OR emulation gap. `dump_guest_code` in `runtimeLinker.cpp` now printf's live bytes at the exception address on next occurrence (compares vs file).

**Perf (the actual goal):** streaming grinds at ~0.2 fps (`DRAIN/s pause≈1–3.5 s/s` post-coalescing, was 7–16). Not yet known whether the level finishes loading — no run survived past ~17 s of streaming (best: frame ~294). Note: my new `Synchronize*` paths call `Transfer::WaitForQueueIdle()` per sync — watch their cost when stability improves.

---

## 4. Ordered plan

1. **Finish the probe** (§2): redo `xfer_qidle` edit in transfer.cpp (exact text above; check waitWatch include), add `rb_wait` scope in `ReadbackWorker::Request` wait loop. `build.bat prod`. Run: `wsl --shutdown; .\drive_gta3.ps1 -IntroSeconds 40 -PostStartSeconds 60 -LogTag pin1`. Read `_gta3_logs\gta3_pin1.out.log` stall dumps — the primitive AND its holder will be named by scope state.
2. **Fix the deadlock at the root** based on the named primitive. Leading candidate fix: texture readback inline on the CP thread (mirror the buffer readback's proven pattern) instead of the cross-thread Ready-wait; keep the adopted-pause as the nested-lock guard either way.
3. **§3.2 abort:** map PLT #0x4d/#0x50 to import names (line 78/81 of `_work/eboot_imports.txt`; if line order ≠ PLT order, resolve via the emulator's NID registry). That names the failing subsystem; then trace what the 70 KB copy (`arg2=0x11280`) is and why its destination/source is inconsistent.
4. **`c000001d`:** on next occurrence compare live bytes (`KYTY_DIAG ... code:` line) vs file; if corrupted, hunt the misdirected write (prime suspect class: cache `WriteBacking` to a stale image range).
5. **Perf:** after stability, re-measure DRAIN/s (`pause`/`waitidle` < 1 s/s target) and watch `Synchronize*`/`WaitForQueueIdle` costs; level-load completion is the milestone, then fps.
6. **Tests:** update the 3 red `BufferImageWrite` expectations in `tests/ShaderRecompilerComputeTests.cpp` (~lines 13144/13159/13174: "target unformatted"→`SynchronizeRenderTarget`, "storage unformatted"→`SynchronizeStorageTexture`, "depth unformatted"→`SynchronizeDepthTarget`) + add partial-raw cases. Known pre-existing red: `SampledColorViews storage` death-case (independent of our changes).
7. **Strip instrumentation** (worklog §7 list incl. `g_commit_on_fault_blocks` unbounded vector) before any final fps measurement; **commit work**; update `docs/gta3-worklog.md` (its §1 "committed vs working tree" and §8 next-steps are stale as of the 3 new commits).

---

## 5. Operating rules (from the user, binding)

- No native logging (`--printf-direction Silent` always; diagnostics only as `std::printf` "KYTY_DIAG ..."). Native logs spam + change race timing.
- Short watches: `-PostStartSeconds 60–90` (failures hit at ~10–18 s past Start; survival by ~60–90 s proves a fix).
- `wsl --shutdown` before each run.
- `Common::Mutex` ≠ std::mutex (use `Common::LockGuard` / manual `Lock()`/`Unlock()`); no `windows.h` in shared headers (macro pollution — implementation in `.cpp`).
- Builds: `build.bat prod` (exe `_Build/windows-prod/install/kyty_emulator.exe`), `build.bat test` (suite). Symbolize: `llvm-symbolizer --obj=_Build/windows-prod/install/kyty_emulator.exe 0x140...`. Offline shader repro: `_Build/windows-prod/shader_recompile_file.exe <bin> ps [watchdog|dumpcfg]` (input from `_Shaders/precmp/` via `KYTY_DUMP_PRECMP=1`).
