# GTA III DE — Inline Readback Refactor: Handoff & Field Manual

**Audience:** a fresh agent with zero memory of previous sessions. Read this first, then `docs/gta3-worklog.md` (session 2) and `docs/gta3-definitive-issues.md` (session 1) if you need deep history. `docs/gta3-continuation-plan.md` is superseded by this document for everything related to the readback deadlock.

**Mission (the project's goal, from the user):** make GTA III: The Definitive Edition (PPSA03527) **load a level with a good frame rate** on the KytyPS5 emulator. Not "fix every crash" — load the level, then make it fast. Keep your eyes on that.

**Binding user rules (violating these wastes sessions):**
1. **Document everything you do inside md files.** Keep this file (or a successor) current: facts, run outcomes, diffs-in-flight, next steps. A new agent must be able to resume from docs alone.
2. **Base changes on facts and real tests with the emulator and the game**, not theory. Every claim needs a log line, a measurement, or a disassembly.
3. No native logging: always run with `--printf-direction Silent` (the driver script does this). Diagnostics are `std::printf("KYTY_DIAG ...")` only. Native logs spam and change race timing.
4. `wsl --shutdown` before each game run.
5. The user has given **full freedom to refactor** the GPU fault/pause/readback logic. They do not hold the old logic sacred. They only care that the game works.

---

## 1. Repo state at handoff (read carefully — the tree is mid-refactor)

**Branch:** `fix/gta3-level-load`. **Top commit:** `19ec9fb` (probe scopes + continuation plan).

**Uncommitted work in the tree (all intentional, do NOT revert):**
- The user's earlier fixes (adopted-pause in graphicsRun, readback-worker adoption, pageManager read-fault handling, waitWatch capture dedup).
- WaitWatch pinpoint scopes: `dmem_lock` (memoryAddressSpace.inc), `mt_dl`/`mt_ul` (memoryTracker.h), `bc_publish` (bufferCache.cpp), `xfer_qidle` (transfer.cpp), `rb_wait` (textureCache.cpp — will be deleted by the refactor below).
- **BufferCache inline-readback refactor: DONE** (`src/graphics/host_gpu/renderer/bufferCache.h`, `bufferCache.cpp`). `ReadbackWorker` (thread + state machine) replaced by synchronous `Readback` (see §5). Call sites updated (`Begin`/`Complete`/`Release`); `m_readback->Prepare()` call in `ObtainBuffer` removed; duplicate `gpu_rbreq` scope removed.
- **TextureCache header: DONE** (`textureCache.h` — `ReadbackWorker` renamed to `ImageReadback`).
- **TextureCache implementation: DONE** (`textureCache.cpp` — spliced via `_work/splice_texture.py`, call sites `Begin`/`IsReady`/`Complete`/`Release`). **The tree builds clean** (`build.bat prod`, first try). §6 below is the record of what was done, kept for the next reader; only §6's step 6 (verification runs) remains.

**Untracked helpers:** `_work/` (splice scripts, eboot slices), `_gta3_logs/`, `drive_gta3.ps1`, `tests/ShaderRecompileFile.cpp`.

---

## 2. The problem being killed (pin1 run, all facts verified)

**Symptom:** mid level-load the frame freezes forever (frame counter stuck, e.g. frame 201 for 60+ s). No crash, no GPU hang (`gpu: not blocked on WAIT_REG_MEM`, GPU fences fine — the `fence-stall` probe in `CommandBuffer::WaitForFenceOnly` never fires).

**The deadlock anatomy (run `_gta3_logs/gta3_pin1.out.log`, WAITWATCH stall dumps):**

- `GpuWorker` (the CP/command-processor thread): `state=rb_wait arg0=0x204e6e0000 held 5+ s`. It faulted on a GPU-dirty image page and waits **cross-thread** for the texture readback worker's `State::Ready`, while holding `m_resource_mutex` (FaultScope) + `TextureCache::m_fault_mutex`. Symbolized stack: `HandleFault → PageManager::HandleFault → FaultThunk → BufferCache::InvalidateMemory → TextureCache::InvalidateMemory → GraphicsRunFinishScheduler → ReadbackWorker::Request`.
- `TextureReadback` (the texture readback worker thread): `state=rb_download arg0=<same page> held 5+ s`, parked in an ntdll wait. Proven by disassembly (PDB has no line info, so this was done by hand — method in §9):
  - `0x140052f20` = return address after `call Common::Mutex::Lock` at `0x140052f1b`, inside the **`FaultSafeCacheLock` constructor** (`bufferCache.cpp:117-121` — identified by its TLS read of `g_cache_lock_owner`, the 58-char "recursive cache lock acquisition" fmt string, and `DbgExitHandler(file, line=118)`).
  - Its caller's return `0x14005c902` exactly matches the call site `0x14005c8fd` inside **`BufferCache::HasPageOverlap`** (`bufferCache.cpp:1213-1227` — identified by the `& -0x1000` PageOverlaps math right after the lock).
  - **So the worker was blocked acquiring `BufferCache::m_mutex` in `HasPageOverlap`, called from `DownloadColorImage` (`textureCache.cpp:747`).**
- A `?` thread in `state=gpu_pause_lock` held the same 5+ s — the shared-pause leader/joiner stuck because `PauseSubmissionsShared`'s leader waits for the CP worker to park, and the CP worker is in `rb_wait` forever.
- The `BufferCache::m_mutex` holder is invisible in the dump (no WaitWatch scope on that path), but structurally the only party that holds it across a thread-dependent wait is the **buffer readback worker**: its `Run()` holds `FaultSafeCacheLock(cache.m_mutex)` from lookup through `WaitForFenceAndReset` **and the subsequent `state.wait` for `State::Completed`** (`bufferCache.cpp` old lines 412-487). The Completed signal depends on the buffer-readback requester finishing its fault, which is wedged behind the same pause window / resource mutex.

**The cycle:** GpuWorker waits for TextureReadback → TextureReadback waits for `BufferCache::m_mutex` → buffer readback worker holds it waiting for its requester → requester/pause-leader waits for the GpuWorker to park. Four parties, four waits, cycle closed.

**This was the third distinct deadlock shape from the same architecture** (after the label-callback cycle and the tracker re-entry). The code itself admits it (`gpuResourceManager.cpp:74-77`: "A real fix must drain only for sync-relevant faults, which is a fault/lock redesign"). Whack-a-mole is proven; that's why the user authorized the redesign.

---

## 3. Facts the refactor stands on (do not re-verify)

1. **All page-fault handling is serialized by `GpuResourceManager::m_resource_mutex`** (`FaultScope` wraps `m_page_manager.HandleFault` on all three paths: CP thread `gpuResourceManager.cpp:48`, label callback `:69`, external threads `:80`). No two faults ever resolve concurrently.
2. **All three fault phases (Invalidate → Complete → Release) run synchronously on the faulting thread** inside `PageManager::HandleFault` (`pageManager.cpp:592-724`).
3. Transfer downloads are self-contained: `ExecuteImmediateCommands` creates a local `CommandBuffer`, submits via the shared queue (`queue_mutex`, a leaf lock), and waits on its **own** Vulkan fence (`transfer.cpp:105-112`). The staging buffer self-locks (`ReusableStagingBuffer::m_mutex`, `transfer.cpp:316`). GPU fences retire independently of any CPU thread.
4. The only GPU locks a fault path can need are leaf locks: staging mutex, `queue_mutex`, tiler mutex, tracker `m_access_mutex`, dmem `m_mutex`, cache mutexes — **as long as nobody holds them across a cross-thread wait** (that was the bug).
5. `WriteBacking` (kernel `DirectMemoryBacking::TryTransferBacking`) writes via the backing alias; its memcpy can only hit *uncommitted* backing → `KernelCommitReservedOnFault` (`memory.cpp:3459`), which touches **no GPU locks** (only `g_memory_operation_mutex`).
6. The old readback workers' own pause handling was dead code in practice: every requester was already prepaused (CP thread → `command_thread=true`; external fault → `GraphicsRunSubmissionLock` held at `gpuResourceManager.cpp:79`).
7. `fence-stall` probe (bounded `WaitForFenceOnly`, prints after 4 s) has never fired in any run — GPU work is never the blocker.

---

## 4. The refactor — design and why it wins

**One invariant:** *Page-fault handling is fully synchronous on the faulting thread. While holding any lock, a thread waits only on its own GPU fences — never on another thread's progress.*

Both readback worker threads are deleted. A readback is just: the faulting thread (already serialized by `m_resource_mutex`) submits the download copy itself and waits on its own fence. The state machines (`Idle/Claimed/Requested/Ready/Installed/Completed` + atomic waits) collapse to a 3-state phase (`None/Ready/Installed`) that merely validates the phase protocol.

**Wins, concretely:**
- The `rb_wait` cross-thread wait (deadlock edge 1) ceases to exist — there is no worker to wait for.
- The buffer readback worker's `m_mutex`-held-across-`state.wait` (deadlock edge 2) ceases to exist — the copy, fence, and memcpy all run inline on the faulting thread, and the lock is released when the phase ends.
- The pause leader can no longer wedge on a faulting CP thread: the CP thread's fault resolves synchronously (bounded by its own GPU fence), then it returns to the PM4 loop and parks normally.
- ~400 lines of thread/state-machine code deleted; the deadlock-prone surface shrinks to leaf locks. Debugging becomes: whoever holds a lock is *doing something*, never *waiting for someone*.

**What does NOT change (deliberately):** the shared-pause coalescing (`PauseSubmissionsShared` — it measured well: pause 7–16 s/s → 1–3.5 s/s), the phase protocol in the caches' `InvalidateMemory`, the tracker, the tiler, the download math. Minimal blast radius.

**Known pre-existing hazard NOT made worse, NOT fixed (note for later):** `UnmapGpuRange` (`memory.cpp:94-107`) runs under the global `g_memory_operation_mutex` and takes the submission pause inside; a `WriteBacking` commit-fault needs the same mutex. A guest thread doing GPU-unmap while the CP thread readbacks could form a new cycle. Not observed in any run so far; fix only if a stall dump names it.

---

## 5. What was done (BufferCache — complete)

`bufferCache.cpp`: `struct BufferCache::ReadbackWorker` (old lines 176-508, thread + 10-state machine) replaced by `struct BufferCache::Readback`:

- `EnsureResources()` / `ReleaseResources()` — the readback Vulkan buffer + `CommandBuffer`, created lazily on first fault (was: created on the worker thread at init). Ctor no longer spawns a thread; dtor frees resources synchronously.
- `Begin(access, vaddr, size)` — the old `Request`'s safety EXITs (minus the cross-thread bits) + the old `Run()` body: `FaultSafeCacheLock` → find cached buffer → build copies/barriers from `m_gpu_modified_ranges` → **`command->Execute(); WaitForFenceAndReset(); memcpy` inline on the faulting thread** → `phase = Ready`. The `gpu_rbreq` WaitWatch scope wraps the locked section.
- `Complete(...)` — unchanged logic (WriteBacking per range + `CompleteCpuFault(downloaded=true)`), minus the state machine; `None → return false`, `Ready → Installed`.
- `Release(...)` — `None → return`; else validate and `m_gpu_modified_ranges.Subtract(page)` under a short `FaultSafeCacheLock` (was: done by the worker after the Completed wait — the deadly part).
- `InvalidateMemory`: `Request` → `Begin`; the `m_readback->Prepare()` call in `ObtainBuffer` deleted.

## 6. What remains (TextureCache — DO THIS NEXT)

`textureCache.cpp` still has the old `ReadbackWorker` at line 352. The splice script `_work/splice_texture.py` is ready: it replaces 6 spans by verified anchors and keeps the two big download methods (`DownloadDepthTarget`, `DownloadColorImage`, ~lines 492-815) untouched. It loads six sidecar fragments `_work/tex_*.cppfrag` — **create them with the contents below, then run `python _work/splice_texture.py`.** (Everything in `_work/` is CRLF-aware; the script asserts every anchor before writing.)

**`tex_decl.cppfrag`** (replaces struct decl + State + ctor/dtor, keeps `ReadbackTransfer`):

```cpp
struct TextureCache::ImageReadback {
	struct ReadbackRange {
		uint64_t address;
		uint64_t size;
	};
	struct ReadbackTransfer {
		std::array<ReadbackRange, 2> ranges {};
		uint32_t                     count = 0;

		void Add(uint64_t address, uint64_t size) {
			if (address == 0 || size == 0 || count == ranges.size()) {
				EXIT("TextureCache: invalid image readback transfer range\n");
			}
			ranges[count++] = {address, size};
		}
		[[nodiscard]] std::span<const ReadbackRange> Ranges() const {
			return {ranges.data(), count};
		}
	};

	enum class Phase : uint32_t { None, Ready, Installed };

	explicit ImageReadback(TextureCache& owner): cache(owner) {}

	// Synchronous readback. Page-fault handling is serialized by the resource mutex
	// (GpuResourceManager::HandleFault), so the download runs on the faulting thread itself:
	// no worker thread and no cross-thread waits. While any lock is held this thread waits
	// only on its own GPU fence, which retires independently of other threads.
```

**`tex_begin.cppfrag`** (replaces `Request`; absorbs the old `Run()` body lines 836-895). Note it holds `cache.m_lock` across the whole fault via `lock_held` — **required**, because the four non-fault download callers (`textureCache.cpp:1104/1356/1421/1640`, image recreation/retirement on draw paths) use the same `download`/`guest` member buffers and must serialize against a pending fault readback:

```cpp
	void Begin(PageFaultAccess fault_access, uint64_t fault_vaddr, uint64_t fault_size) noexcept {
		const bool command_thread            = GraphicsRunIsCommandProcessorThread();
		const bool submissions_prepaused_now = GraphicsRunSubmissionLockHeld() || command_thread;
		const bool unsafe_gpu_lock = GraphicsRunGpuLockHeld() && !submissions_prepaused_now;
		if ((fault_access != PageFaultAccess::Read && fault_access != PageFaultAccess::Write) ||
		    unsafe_gpu_lock || LabelInCallback() || g_texture_cache_lock_owner != nullptr ||
		    g_texture_fault_owner != &cache) {
			EXIT("TextureCache: unsafe image readback request, access=%u command_thread=%d "
			     "submission_lock=%d gpu_lock=%d label_callback=%d cache_lock=%p fault_owner=%p\n",
			     static_cast<uint32_t>(fault_access), command_thread,
			     GraphicsRunSubmissionLockHeld(), GraphicsRunGpuLockHeld(), LabelInCallback(),
			     g_texture_cache_lock_owner, g_texture_fault_owner);
		}
		if (phase != Phase::None) {
			EXIT("TextureCache: reentrant image readback request, vaddr=0x%016" PRIx64 "\n", vaddr);
		}
		access = fault_access;
		vaddr  = fault_vaddr;
		size   = fault_size;

		std::optional<GraphicsRunSubmissionLock> submissions;
		if (!submissions_prepaused_now) {
			submissions.emplace();
		}
		Kyty::WaitWatch::Scope rb_scope("rb_download", vaddr, size); // KYTY_DIAG
		lock_held            = std::make_unique<FaultSafeTextureLock>(&cache, cache.m_lock);
		CachedImage* selected = cache.FindGpuReadbackPageCandidateLocked(vaddr, size);
		const bool   render_target =
		    selected != nullptr && selected->kind == CachedImage::Kind::RenderTarget;
		const bool storage_texture =
		    selected != nullptr && selected->kind == CachedImage::Kind::StorageTexture;
		const bool depth_target =
		    selected != nullptr && selected->kind == CachedImage::Kind::DepthTarget;
		if ((!render_target && !storage_texture && !depth_target) || !selected->gpu_modified ||
		    selected->buffer_modified || selected->image == nullptr) {
			EXIT("TextureCache: unsupported GPU image readback owner, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 " access=%u image=%p kind=%u gpu_modified=%d "
			     "buffer_modified=%d vulkan_image=%p\n",
			     vaddr, size, static_cast<uint32_t>(access), static_cast<const void*>(selected),
			     selected != nullptr ? static_cast<uint32_t>(selected->kind) : UINT32_MAX,
			     selected != nullptr && selected->gpu_modified,
			     selected != nullptr && selected->buffer_modified,
			     selected != nullptr ? static_cast<const void*>(selected->image) : nullptr);
		}

		transfer = depth_target ? DownloadDepthTarget(*selected)
		                        : DownloadColorImage(*selected, false);

		if (!cache.m_memory_tracker.IsRegionGpuModified(vaddr, size)) {
			EXIT("TextureCache: readback fault page is not GPU-modified, addr=0x%016" PRIx64
			     " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
		const auto fault_page = vaddr & ~(TRACKER_PAGE_SIZE - 1);
		const auto page_end   = fault_page + TRACKER_PAGE_SIZE;
		for (const auto& range: transfer.Ranges()) {
			const auto range_end = range.address + range.size;
			if (fault_page < range_end && page_end > range.address) {
				if (fault_page > range.address) {
					cache.m_memory_tracker.ForEachDownloadRange<true>(
					    range.address, fault_page - range.address,
					    [](uint64_t, uint64_t) noexcept {});
				}
				if (page_end < range_end) {
					cache.m_memory_tracker.ForEachDownloadRange<true>(
					    page_end, range_end - page_end, [](uint64_t, uint64_t) noexcept {});
				}
			} else {
				cache.m_memory_tracker.ForEachDownloadRange<true>(
				    range.address, range.size, [](uint64_t, uint64_t) noexcept {});
			}
		}
		this->selected = selected;
		phase          = Phase::Ready;
	}
```

**`tex_complete.cppfrag`:**

```cpp
	[[nodiscard]] bool Complete(PageFaultAccess fault_access, uint64_t fault_vaddr,
	                            uint64_t fault_size) noexcept {
		if (phase == Phase::None) {
			return false;
		}
		if (phase != Phase::Ready) {
			EXIT("TextureCache: active image readback has invalid completion state %u\n",
			     static_cast<uint32_t>(phase));
		}
		if (access != fault_access || vaddr != fault_vaddr || size != fault_size) {
			EXIT("TextureCache: mismatched active image readback completion\n");
		}
		phase = Phase::Installed;
		return true;
	}
```

**`tex_isready.cppfrag`:**

```cpp
	[[nodiscard]] bool IsReady(PageFaultAccess fault_access, uint64_t fault_vaddr,
	                           uint64_t fault_size) const noexcept {
		return phase == Phase::Ready && access == fault_access && vaddr == fault_vaddr &&
		       size == fault_size;
	}
```

**`tex_release.cppfrag`** (absorbs the old `Run()` post-completion block, lines 906-924):

```cpp
	void Release(PageFaultAccess fault_access, uint64_t fault_vaddr, uint64_t fault_size) noexcept {
		if (phase == Phase::None) {
			return;
		}
		if (phase != Phase::Installed) {
			EXIT("TextureCache: active image readback has invalid release state %u\n",
			     static_cast<uint32_t>(phase));
		}
		if (access != fault_access || vaddr != fault_vaddr || size != fault_size) {
			EXIT("TextureCache: mismatched active image readback release\n");
		}
		for (const auto& range: transfer.Ranges()) {
			if (cache.m_memory_tracker.IsRegionGpuModified(range.address, range.size)) {
				EXIT("TextureCache: completed image readback retained GPU ownership, "
				     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     range.address, range.size);
			}
			if (cache.m_buffer_cache.HasPageOverlap(range.address, range.size)) {
				// BufferCache completes the same write fault before TextureCache's release
				// phase, so the faulting page may already be CPU-dirty even though the guest
				// instruction has not executed yet. Preserve that state while publishing the
				// complete image contents downloaded immediately before fault completion.
				cache.m_buffer_cache.PublishImageBacking(range.address, range.size, true);
			}
		}
		selected->gpu_modified = false;
		selected               = nullptr;
		phase                  = Phase::None;
		lock_held.reset();
	}
```

**`tex_members.cppfrag`:**

```cpp
	TextureCache&                         cache;
	std::unique_ptr<FaultSafeTextureLock> lock_held;
	CachedImage*                          selected = nullptr;
	ReadbackTransfer                      transfer {};
	PageFaultAccess                       access   = PageFaultAccess::Unknown;
	uint64_t                              vaddr    = 0;
	uint64_t                              size     = 0;
	Phase                                 phase    = Phase::None;
	std::vector<uint8_t>                  download;
	std::vector<uint8_t>                  guest;
```

**Then:**
1. `python _work/splice_texture.py` (asserts all anchors; prints old/new line counts).
2. In `TextureCache::InvalidateMemory` (~line 4350): `m_readback->Request(access, vaddr, size);` → `m_readback->Begin(access, vaddr, size);`. The `IsReady`/`Complete`/`Release` call sites keep their signatures.
3. Delete the now-dead `rb_wait` WaitWatch scope (it lived inside the old `Request` — the splice removes it).
4. `GraphicsRunAdoptedSubmissionPause` becomes unused — leave the class in `graphicsRun.{h,cpp}` (harmless) or remove it; your call.
5. `build.bat prod`. Fix compile errors (likely: `std::optional`/`std::unique_ptr` includes are already there; `FaultSafeTextureLock` is defined earlier in the same file).
6. Run the game twice (§7): `pin2a`/`pin2b`. Success = no `rb_wait`/stall with held_ms climbing past the previous ~17 s wall; streaming continues; then measure DRAIN/s for perf.

---

## 7. Build, run, debug (the daily loop)

```
build.bat prod            # exe: _Build\windows-prod\install\kyty_emulator.exe
build.bat test            # builds + runs the test suite in _Build\windows-nolauncher
```

Headless game driver (injects Win32 input; window title shows `frame: N, fps: F`):

```
wsl --shutdown            # free system commit before each launch — do not skip
.\drive_gta3.ps1 -IntroSeconds 40 -PostStartSeconds 60 -LogTag TAG
```

- Logs: `_gta3_logs\gta3_TAG.out.log` (+ `.err.log`). Failures historically hit ~10-18 s past Start; surviving 60-90 s proves a fix. Prove stability with **2+ consecutive stall-free runs** — failure modes are timing-dependent (deadlock ~25%, abort ~50%).
- Grep keys: `DRAIN/s`, `KYTY_DIAG`, `WAITWATCH (stall`, `host stack`, `name=GpuWorker`, `--- Error ---`, `Num compiled`, `Guest abort`, `state=(rb_|pm_|gpu_|mt_|dmem_|bc_|xfer_|cp_fault)`.
- Exit codes: `321` = clean EXIT/DbgExit; `-1073741787` = 0xC0000005; `c000001d` = illegal instruction.
- **Symbolize a WAITWATCH host stack:** `llvm-symbolizer --obj=_Build/windows-prod/install/kyty_emulator.exe 0x140...` (PDB has function symbols only, no line info). If attribution is ambiguous ("nearest symbol"), disassemble with `llvm-objdump -d --start-address=0x… --stop-address=0x…` and match call targets — that is exactly how `FaultSafeCacheLock`+`HasPageOverlap` were proven (§2); full method there.
- **Offline shader repro:** `_Build/windows-prod/shader_recompile_file.exe _work/hang_ps.bin ps 60` (or `... ps dumpcfg`). Dumps come from `_Shaders/precmp/` with `KYTY_DUMP_PRECMP=1`.
- **Offline eboot disassembly:** SELF payload is plain ELF at `eboot.bin` offset `0x1a0`; extracted text at `_work/eboot_text.bin`; `objdump -D -b binary -m i386:x86-64 --adjust-vma=0x0 --start-address=0x… _work/eboot_text.bin`.

**Editing gotchas that already cost time — avoid them:**
- Source files are **CRLF**. The Read tool shows LF; Edit writes CRLF back for pure-CRLF files. For edits >30 lines, don't fight `Edit` whitespace — write an anchored Python splice (see `_work/splice_texture.py` for the pattern: find unique anchors, assert, splice, verify).
- `Common::Mutex` is not `std::mutex` (use `Common::LockGuard` / `Lock()`/`Unlock()`).
- No `windows.h` in shared headers (macro pollution) — implementation in `.cpp`.

---

## 8. Open fronts after the refactor (priority order)

1. **Verify the deadlock is gone** (§6 step 6). If a stall recurs, the dump now names a leaf-lock holder *doing* something — fix that specific lock ordering, or extend the same synchronous treatment.
2. **`Guest abort()` (~50% of runs, deterministic site):** mid-instruction inside an optimized `memcpy` at `eboot+0x1837ad3`; registers: `arg0(rdi)=eboot+0x643b4a8` (BSS, struct of 3 pointers to zeroed buffers), `arg2=0x11280` (70 KB copy). The function calls PLT import **#0x4d** 10× before the copy, **#~0x50** after. Resolve names: `_work/eboot_imports.txt` line order likely == PLT order (UNVERIFIED) → lines 78 (`0x4d+1`) and 81 (`0x50+1`); cross-check NIDs against registered NIDs in `src/libs/`. Zero-fill hypothesis already REFUTED (all `commit-on-fault-filled=no`). Abort-diag prints are live in `libC.cpp`.
3. **`c000001d` illegal instruction (once, coal8):** at `eboot+0x1b0bae5`; file bytes `4c 89 f7` are legal → in-memory text corruption or emulation gap. `dump_guest_code` in `runtimeLinker.cpp` prints live bytes on the next occurrence.
4. **Perf (the actual goal):** streaming grinds at ~0.2 fps; best frame ~294, never past ~17 s of streaming. After stability: re-measure DRAIN/s (`pause`/`waitidle` < 1 s/s target), watch the cost of `Synchronize*`→`Transfer::WaitForQueueIdle` per sync, and shader-compile cost (the fixed §2 shader emits 205184 SPIR-V words; `_pipeline_cache.bin` persists across runs).
5. **Tests:** update 3 red `BufferImageWrite` expectations in `tests/ShaderRecompilerComputeTests.cpp` (~lines 13144/13159/13174: "target unformatted"→`SynchronizeRenderTarget`, "storage unformatted"→`SynchronizeStorageTexture`, "depth unformatted"→`SynchronizeDepthTarget`) + partial-raw cases. Known pre-existing red: `SampledColorViews storage` death-case (independent).
6. **Strip instrumentation before final fps measurement** (list in `docs/gta3-worklog.md` §7; note `g_commit_on_fault_blocks` grows unboundedly). **Commit work** — nothing since `19ec9fb` is committed. Keep docs current per rule §0.1.

---

## 9. Session 3 (2026-07-28): inline readback verified — deadlock DEAD, now a dual-ownership tail

**Headline: the readback deadlock is gone.** With the inline refactor finished, GTA III now *streams* the level (frame counter climbs 205→274+ under load) instead of freezing forever. No `rb_wait`, no stall dumps that never clear. The refactor's core thesis held. What remained were three *new* classes of failure the inline model exposes, fixed/triaged below.

### 9.1 The `g_in_fault_resolution` wall (FIXED — this was the real reason the workers existed)

The handoff design (§3–§5) missed that the async readback workers were load-bearing for more than the cross-thread wait: they ran the readback's **mapping queries and page-watcher mutations on a thread where `g_in_fault_resolution` is clear**. That thread-local (`pageManager.cpp`) makes a whole family of `PageManager` methods refuse to operate during resolution. Moving the readback inline onto the faulting thread trips them in sequence:

- **`IsMapped`/`HasAnyMapping` return `false` during resolution** → `MemoryTracker::RequireMapped` (called by every `IsRegion*Modified`) EXITs "range is not mapped". **Fix:** made those two reads answer truthfully during resolution (they only take page locks, and `HandleFault` holds none across its callbacks; the sole in-resolution caller is the inline readback). `pageManager.cpp:300,320`.
- **`UpdatePageWatchers` FailFasts during resolution** → the readback's `ForEachDownloadRange<true>` removes GPU watches on downloaded pages (`RegionManager::ApplyGpuProtection`, `memoryTracker.h:80`). This is the *exact* bookkeeping the old worker did off-thread; the `HandleFault` epilogue (`pageManager.cpp:694`) then sees watchers already zeroed and takes the benign branch. **Fix:** `PageManager::FaultReadbackScope` (thread-local depth counter, `pageManager.{h,cpp}`) narrowly permits watcher mutation *only* while an inline readback is on the stack; every other in-resolution mutation still FailFasts. Scopes are placed in `TextureCache::ImageReadback::Begin`/`Release` and `BufferCache::Readback::Begin`/`Release`.

Verified against the code: `WriteBacking` writes the backing **alias** (`m_backing_base+offset`, `memoryAddressSpace.inc:515`), never `BeginBackingWrite`, so the `BackingWrite` resolution guard is a different (unmap) path — not a readback landmine.

### 9.2 `FaultSafeCacheLock` recursion — same as the §2 deadlock, now on one thread (FIXED)

The CP thread (`GpuWorker`) inside `BufferCache::ObtainBufferForImage` holds `m_mutex`, then faults on a GPU-owned image page it was about to read; the inline texture readback calls `BufferCache::HasPageOverlap` → tries to re-take `m_mutex` → `FaultSafeCacheLock` recursion EXIT. (Proven with a `_ReturnAddress` diag: `outer_ra=ObtainBufferForImage`, `inner=HasPageOverlap`, `thread=GpuWorker`, `in_readback=1`.) This is literally the §2 four-party deadlock collapsed onto a single thread — the old worker would have blocked on `m_mutex` here forever.

**Fix:** `FaultSafeCacheLock` is now **reentrant when `PageManager::InFaultReadback()`** (`bufferCache.cpp:113`). The outer op is suspended mid-instruction (not mid-mutation of `m_buffers`), and the ownership transfer the readback performs is exactly what the outer op needs before it resumes, so the readback proceeds on the already-held lock instead of dead-locking on it. Any *non-readback* re-entry still EXITs. Also dropped the now-obsolete `g_cache_lock_owner != nullptr` rejection in `BufferCache::Readback::Begin`'s context guard.

### 9.3 The remaining tail — dual-ownership aliasing (OPEN, this is the new front)

With the above fixed, streaming dies ~20–40 s in at a *rotating* set of sites (each ~10–25 %/run — timing-dependent). Symbolized, they are all the **same root cause: memory that is simultaneously a GPU-dirty image (render target) AND a cached buffer**, i.e. the two caches both claim it:

- **`memoryTracker.h:151` "re-entered from upload callback"** (`same=1`): an outer *buffer upload* (`ForEachUploadRange`, region spinlocks held) reads source memory that is a GPU-dirty image; the fault's inline texture readback calls `m_buffer_cache.IsRegionCpuModified` on the **same tracker/region** → genuine spinlock reentrancy (cannot just relax the guard — the region lock would self-deadlock). Only fires when `HasPageOverlap` is true, i.e. a real image∩buffer overlap.
- **`bufferCache.cpp:1057` "GPU copy aliases target pages"** and **`textureCache.cpp:1252` "unsupported sampled/depth-target alias"**: genuinely-unsupported aliasing (a GPU buffer copy dst / a sampled texture overlapping a render/depth target). Pre-existing cache-model gaps, not caused by the refactor.

**The real fix direction:** resolve dual ownership *before* the CP thread reads the memory, i.e. extend the "materialize the overlapping image before taking the lock / before the upload callback" pattern that `ObtainBuffer` already uses (`bufferCache.cpp:582-596`, `PrepareGpuBufferRead`/`InvalidateMemoryFromGPU`) to `ObtainBufferForImage`, the upload paths, and `CopyBuffer`. That removes the mid-operation fault entirely, so no reentrancy arises. This is the next front and is likely *the* thing standing between "streams" and "level loaded".

- **`Guest abort()` at `libC.cpp:314`/`eboot+0x1837ad3`** still occurs (§8.2) — independent of the readback work.
- **Perf** unchanged and severe: ~0.1–0.9 fps during streaming, `DRAIN/s pause≈5272 ms/s` (multi-thread synchronous drains). Still the §8.4 async-redesign problem; correctness first.

**Files touched this session (all uncommitted before the checkpoint commit):** `pageManager.{h,cpp}` (truthful reads, `FaultReadbackScope`), `bufferCache.cpp` (reentrant `FaultSafeCacheLock`, readback scopes, context guard), `textureCache.cpp` (readback scopes). No diagnostics left in the tree.

---

## 10. Session 3 part 2 (2026-07-28): the perf wall is the per-readback worker park

The user asked to prioritise frame rate (faster iteration → catch crashes sooner). Measured it properly first by adding `readbacks=`, `fTot=` (total faults) and `fNew=` (newly-seen pages) to the `DRAIN/s` line.

**What the numbers actually say (streaming, per ~1 s report):**
- The old "≈99.6 % of faults need no readback" claim is **false for GTA III streaming**. Readbacks are **~5–40 %** of faults depending on phase (e.g. one phase `readbacks=50/faults=87`, another `readbacks=4/faults=89`).
- Massive **fault thrash**: reports with `fNew=0` (zero new pages) yet `faults=41–89` — the same pages fault over and over. These are pure invalidations (CPU writing a buffer the GPU has a clean copy of), downloading nothing.
- Cost is dominated by `waitidle` (the worker **park**, `RequestPauseAndWait`): ~1.1–2.4 s per report. `labeldrain`≈0.1 ms and `bufwait`≈10 ms are negligible.

**Fix shipped — drain only for sync-relevant faults** (`gpuResourceManager.cpp` external-thread path): probe under the resource mutex whether the fault will actually download GPU data, and skip the whole submission drain (worker park + fence + label flush) when it won't. The probe is *exact*, not heuristic:
- buffer readback ⟺ page GPU-dirty (`RegionManager::BeginCpuFault` returns `Download` only then) → `MemoryTracker::HasGpuModifiedUnchecked` (new; no `RequireMapped`, so a probe never EXITs);
- texture readback ⟺ `FindGpuReadbackPageCandidateLocked != null`;
- metadata write ⟺ `m_metadata_tracker.HasGpuModifiedUnchecked` (mirrors `InvalidateVirtualGpuWrite`).
GPU-dirty state only changes under the resource mutex (worker transactions + label callbacks all hold it), so the probe exactly predicts `HandleFault`'s Invalidate phase. `TextureCache::FaultWouldReadback` / `BufferCache::FaultWouldReadback` implement the per-cache predicates. Verified stable across ~6 runs (no new crash class, no starvation hang — labels still flush on every readback fault, which never stop during streaming).

**Result: faults taking the drain dropped from ~90–270 to ~2–5 per report — but fps did NOT improve (~0.1–0.9 either way).** Why: the skipped faults were already cheap *joiners* on the shared-pause window; the wall is the readback **leaders** parking the worker. `waitidle` is essentially proportional to `readbacks`, not `faults`. With fewer drains the worker runs longer between them, so each park now waits longer — net wall-clock unchanged.

**So the real fps wall (next front): the per-readback worker park.** Each GPU→CPU readback calls `RequestPauseAndWait` (park the whole GPU worker until its current submission finishes) so the render target's in-progress command buffer is flushed before the download copy. The download copy already runs on the **same `graphics.queue`** as rendering (`Transfer::ExecuteImmediateCommands` → `command.Execute`), so GPU ordering is automatic — the park exists only to flush the worker's *partially-built* buffer, and it waits for **unrelated** draws too. The fix is finer-grained: make a readback wait only for the specific render target's work to be flushed/fenced, not a global worker park. That is a real GPU-sync change (per-resource fence tracking) and is the highest-value perf item, but it must not read partial/stale targets (that would corrupt output and *hinder* crash-hunting), so it needs care, not haste.

**Bottom line for the goal:** even a perfect perf fix won't make the level *load* — streaming still dies on the §9.3 dual-ownership crashes every run (`bufferCache:249/1057`, `memoryTracker:151`, `textureCache:1252`, and the `libC:314` guest abort). Those remain the actual blocker; perf is about iteration speed on top.

**Instrumentation added (strip before any final fps number):** `readbacks=/fTot=/fNew=` on the `DRAIN/s` line (`emulator.cpp`), `readback_count` increments in both readback `Begin`s.

---

## 11. Session 3 part 3 (2026-07-28): the real fps win — texture-cache O(N) scans

The DRAIN work (§10) was a dead end for fps because it targeted the wrong thing. Profiled properly instead: added a **worker-scope sampler** (the watchdog samples `GpuWorker`'s live WaitWatch scope every ~5 ms and prints a histogram as `WORKER/s`), plus `draws=/dispatches=/findTex=/images=` counters. That immediately showed the truth:

1. The GPU worker was **~80 % in `gpu_pm4`** (not parked, not readback-bound) — i.e. CPU-bound recording draws.
2. Only **~200 draws/s** (≈5 ms/draw — absurd). Scoping the draw path: **~70 % of the worker was in `BindDescriptors` → `NativeTexture` → `TextureCache::FindTexture`**.
3. `FindTexture` ran **three full O(N) linear scans over `m_images`** (exact-match, storage-overlap, sampled-overlap), and `images=~2000` during streaming with `findTex=~1000-5000/s`. Quadratic. That was the entire fps wall.

**Fix (committed `aac35e9`, `28d0843`):** every hot exact/overlap lookup was already funnelled through `FindImagesInRegionLocked` → `m_image_owner_index` (an address index) *except* the raw `for (auto& c : m_images)` scans in `FindTexture` / `FindRenderTarget` / `FindDepthTarget` / `FindStorageTexture`. All their match predicates (`Equal`, `IsCompatible*Backing`, `IsCompatibleRenderTargetView`, `EqualStorageBacking`) require `cached.address == info.address`, so the address-index query returns exactly the same candidates — O(overlap) not O(2000). `CachedImage` gained `enable_shared_from_this` so the exact-match paths still return a `shared_ptr` from an index raw pointer.

**Measured impact:**
- FindTexture: **~75 % → ~5 %** of worker wall-clock.
- Draws: **~200/s → ~600-1650/s**; worker now **20-56 % idle**.
- **Streaming fps ~0.1-0.9 → ~2-4 fps; intro ~10 → ~15-17 fps.** Level reaches frame ~316-342 in a fraction of the wall time.

**New bottleneck / next fps front:** the worker is now often idle (guest-submission bound). Its remaining active time is `draw_prepare` (~30 %) — dominated by **`mt_ul` (buffer/texture uploads)** — and `gpu_pm4` (~25 %). Likely next levers: (a) avoid redundant re-uploads (dynamic buffers/textures re-uploaded per draw), (b) the readback worker-park still stalls the worker on the ~40 % of faults that download (see §10). But much of the remaining cost is now genuine guest CPU work under emulation.

**Lesson for the next agent:** when the worker looks "busy", sample its live scope before optimizing — the DRAIN/pause numbers hid a pure-CPU O(N²) texture-lookup problem that had nothing to do with the readback/drain machinery.

---

## 12. Session 3 part 4 (2026-07-28): dual-ownership coherency fix — tracker re-entry ELIMINATED

The consistent streaming crash (`memoryTracker.h:155`, "re-entered from upload callback") was the image∩buffer dual-ownership knot. Root cause pinned exactly: `BufferCache::ObtainBuffer`'s **merge path** (taken when no single cached buffer contains the request) re-uploads *every overlapping cached buffer's whole range* from guest memory, but only `[vaddr,size]` was materialized first. The merged span reaches past that into pages an access-watched GPU render/depth target still owns; the worker's read faults *inside* `ForEachUploadRange` (holding the tracker's `m_access_mutex`) → the inline texture readback's `IsRegionCpuModified` deadlock-guards → EXIT.

**Fix (committed `bb4e059`):** before the cache lock, replicate the merge-range computation under a short probe lock, then `PrepareGpuBufferRead` / `InvalidateMemoryFromGPU` the **full merged span** so no GPU-dirty image remains in it before the upload reads guest memory. `m_buffers` is stable under `m_resource_mutex` and unmutated by materialization (`PublishImageBacking` only marks tracker state), so one pass converges. **Result: `memoryTracker.h:155` went from 3/3 → 0/5 runs.**

**Current crash distribution (post-fix, streaming, frame ~300-350, still ~15fps intro / 3-4fps stream):**
- **`textureCache.cpp:1253` "unsupported sampled/depth-target alias" (~3/5)** — the game samples a texture that *contains* a GPU-dirty depth target with a size mismatch (e.g. sampled `0x1950000` ⊃ depth `0x1800000`, same base). `native_transition` needs `contained && (exact_range || sampled_expansion)`; this is contained but neither exact nor a recognized expansion. Arguably a *correct* EXIT — the sampled bytes beyond the depth target are undefined, so relaxing it risks wrong output. Real fix = implement the containing-depth-sample transition (read back the depth into the larger sampled allocation). Risky feature work.
- **`libC.cpp:314` guest `abort()` at `eboot+0x1837ad3` (~1/5)** — the pre-existing §8.2 abort (70 KB memcpy from a BSS struct of pointers to zeroed buffers). Not caused by any of this session's work (same guest address as prior sessions). Deep guest-behaviour investigation.
- **`textureCache.cpp:2083` (~1/5)** — another texture-alias limitation.

The remaining tail is emulator **feature-completeness** for GTA III's specific aliasing patterns (sampled↔depth/render-target aliases) plus the pre-existing guest abort — each an "unsupported X" guard that EXITs rather than render wrong, so relaxing any needs careful per-case correctness work, not a blanket change.

---

## 13. Session 3 part 5 (2026-07-28): peeling the alias tail (needs visual validation)

With the deadlock/perf/coherency work done, the level-load tail is a sequence of "unsupported alias" EXITs, each a real GTA III GPU-memory reuse pattern. Fixed two so far; each lets the load reach further (frame ~350 → ~411). **Both relax deliberate guards and need on-screen validation — headless testing only proves no-crash, not correct pixels.**

- **`textureCache.cpp:1253` sampled-contains-depth (committed `6f71137`).** A sampled texture fully containing a plain (no stencil/htile, 1 layer) GPU-dirty depth target. The retire path already reads the depth back to *canonical guest backing* and the sampled image uploads from that, so the general `contained` case is handled like the exact/expansion shapes. Low risk (guest backing is the shared canonical form).
- **`textureCache.cpp:2093` render-target reinterpretation (committed `8774246`).** Rebinding the exact footprint as a differently-shaped RT (e.g. 1680x946x8bpe → 3360x1892x2bpe, same bytes). Routed through `MaterializeRetireTarget`, which downloads the old color to guest memory (transient) and makes the **new target fresh** — no preserve-into-new-shape round-trip, so no scramble; the guest renders over it. Correct iff it really is pool reuse (game overwrites); needs validation.

**Current tail (frame ~300-411):** `bufferCache.cpp:1118` GPU-copy (`CopyBuffer` CP-DMA) whose destination aliases a render target `InvalidateMemoryFromGPU` can't retire (deeper classifier work); a residual `memoryTracker.h:155` (~1/3, timing — the merge-span fix cut it from 3/3 but a path remains); and the pre-existing `libC.cpp:314` guest abort. Each is per-case feature work; the risk is silent wrong rendering, so validate the two committed alias fixes on screen before extending further.

---

## §14 Session 4 (2026-07-28) — batchmap ENOMEM fixed + alias tail peeling (frame ~294 → 386)

Branch `fix/gta3-level-load`. Continued peeling the level-load crash gauntlet. All commits `--no-gpg-sign`.

### 14.1 THE BIG WIN: `sceKernelBatchMap` ENOMEM (the old "libC:314" / `LowLevelFatalError [Line: 1270]`)
The dominant multi-session guest abort was NOT a mysterious guest bug. The abort-diag string decodes to:
`LowLevelFatalError [File:Unknown] [Line: 1270]  sceKernelBatchMap failed with error code: 0x8002000c`
i.e. **our `sceKernelBatchMap` returned SCE_KERNEL_ERROR_ENOMEM**. UE4 fatal-errors on that.
- Traced (KYTY_DIAG in `KernelBatchMap2`): the failing entry is `op=0 (MAP_DIRECT) start=0x0 offset=~3GB length=~few×64KB flags=0x10(MAP_FIXED)`.
- Root cause: `KernelBatchMap` forces `MAP_FIXED` on every entry. An entry with `start==0` wants kernel-chosen placement, but `KernelMapDirectMemory`'s fixed path can't honor address 0 (in prod `EXIT_NOT_IMPLEMENTED(in_addr==0)` is a no-op, so it falls through to `ReleaseReservedRange(0,..)` → ENOMEM at memory.cpp:2876). The `CanMapDirect` gate (2739) is NOT the culprit — offset is valid.
- **Fix (commit 235bb47):** in `KernelBatchMap2`, drop `MAP_FIXED` for entries whose `start==0`; the mapping writes the chosen address back into `entry->start`. Entries with explicit addresses keep `MAP_FIXED`. **Result: batchmap ENOMEM eliminated (0/6 runs, was ~40%).**

### 14.2 Texture/depth alias tail peeled (each lets streaming go further)
All in `imageInfo.h`/`textureCache.cpp`, all "buffer-owned image" cases where the classifier was too strict. Pattern: a plain buffer_modified (native-buffer-owned, `!gpu_modified`) image can be retired/discarded cleanly because its bytes are recoverable from the persistent native buffer / guest backing; publish the buffer to backing (`PublishImageBacking`) and clear `buffer_modified` so the clean-retire invariants (`RetireImages`) hold.
- `e40c940`: ClassifyBufferImageWrite StorageTexture — accept a *contained* formatted write into a buffer-owned storage (not only enclosing). RetireSampledTargetAliases — sampled subrange of a buffer-owned plain depth (`buffer_current_depth`).
- `72a6ce5`: publish buffer-owned depth backing before retiring it for a sampled alias (fixes "invalid image retirement kind=3 buffer_modified=1", was 2/4).
- `25f0918`: ClassifyBufferImageWrite DepthTarget — accept a *contained* formatted write into a buffer-owned depth (not only exact). ClassifyStorageSampledOverlap — discard a buffer_modified storage overlapped by a sampled (backing published by FindTexture's ObtainBufferForImage first).
- `235bb47`: storage_discard path publishes+clears buffer_modified before discard (fixes "stale sampled storage owner regained GPU ownership").

### 14.3 CURRENT BLOCKER (frame ~350-386): dual-ownership tracker re-entry (NOT batchmap anymore)
Deterministic `same=1` tracker re-entry, `thread=GpuWorker state=rb_download`, `upload_ra=FindDepthTarget` (~3/6 runs). Mechanism:
- `FindDepthTarget` calls `m_buffer_cache.ObtainBufferForImage(depth region)` at textureCache.cpp:2338 — **before** it resolves overlapping GPU-owned images (which happens after the FaultSafeTextureLock at 2350+).
- A `gpu_modified` color/storage image aliasing the depth's guest memory still holds GPU page protection. ObtainBufferForImage's cpu-dirty upload (bufferCache.cpp:936, `ForEachUploadRange` is_written=false) reads that guest memory → CPU-read fault → TextureCache inline readback (`rb_download`) → `ForEachDownloadRange` → `CheckNotInUploadCallback` re-entry EXIT (memoryTracker.h:165).
- Why it's a hard EXIT not a passthrough: `ForEachUploadRange(is_written=false)` releases region spinlocks before upload_func, but keeps `m_access_mutex` (non-recursive std::mutex) held; a nested `ForEachDownloadRange` would deadlock on it, so the guard is correct. **The fix must PREVENT the fault: materialize/retire the overlapping gpu_modified image(s) BEFORE ObtainBufferForImage reads guest memory in FindDepthTarget.** (Same latent gap exists in FindTexture/FindRenderTarget; depth hits it because the level binds a depth over a region a color/storage pass just wrote.)
- Other tail crashes still seen (1/6 each): "unsupported render-target alias" (RT reclaims a gpu depth, existing_kind=3), "unsupported storage-image byte alias" (storage over gpu depth, gpu=1/1), "color-image readback storage is unsupported" (kind=1 buffer=1 buffer_cpu=1).

### 14.4 Diagnostics added this session (strip before final fps run)
`KYTY_DIAG batchmap-fail` (memory.cpp KernelBatchMap2), `KYTY_DIAG mapdirect-enomem` (KernelMapDirectMemory), RetireImages `_ReturnAddress` caller capture (textureCache.cpp), all under the existing `KYTY_DIAG` convention.
