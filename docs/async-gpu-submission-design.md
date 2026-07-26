# Async GPU submission / finer-grained fault handling — design & phased plan

Status: **design draft** (no implementation). Author: investigation on `fix/gta3-menu-start-crash`.

## 1. Problem

Guest CPU page-faults on GPU-tracked memory each trigger a **full GPU drain**. During
asset-streaming (e.g. GTA III level-load) this is tens of thousands of drains/second, collapsing
the emulator to ~0.15 fps. The game never finishes loading in practical time. This is the
load-bearing "synchronous GPU model" issue.

### Evidence (measured with the wait watchdog, `common/waitWatch.h`)
- Stall dumps show every worker thread (main, `RHIThread`, `PoolThread`s, `RenderThread`) churning
  through `gpu_pause_lock` (`Gpu::PauseSubmissions`) with per-thread activity 10k–48k per 5 s window.
- `gpu_labels_pending = 0` and the CP is not persistently on `WAIT_REG_MEM` during the stall → **not
  a deadlock, a thrash**.
- Fault classification: over ~2100 consecutive faults, `gpu_drains ≈ cpu_faults` (≈1:1) and
  `readbacks ≈ 0` (0.4%). **~99.6% of drains protect no GPU→CPU download.**

### The hot path
`GpuResourceManager::HandleFault` (`gpuResourceManager.cpp`), external (non-CP-thread) branch:
```
GraphicsRunSubmissionLock submissions;   // Gpu::PauseSubmissions(): m_submission_mutex.Lock()
                                         //   + WaitForIdle() + m_gfx_cp->BufferWait() + LabelDrain()
ResourceMutex::FaultScope fault(m_resource_mutex);
m_page_manager.HandleFault(access, fault_vaddr);
```

## 2. Why naive removal fails (the invariants the drain stands in for)

Removing `GraphicsRunSubmissionLock` from the fault path (tested) **hangs the GPU at startup**
(frame 2): the CP blocks on a `WAIT_REG_MEM` for a guest value that is never written. So the drain
currently provides, entangled together:

1. **Cache/tracker serialization** vs the command processor.
   → *Already_ provided by `ResourceMutex::FaultScope`. The drain is NOT needed for this.*
2. **GPU label completion.** `PauseSubmissions` calls `LabelDrain()`, flushing GPU label callbacks
   that (among other things) write guest sync values the CP's `WAIT_REG_MEM` waits on.
3. **CE/DE ring ordering.** The constant engine writes values the draw engine's `WAIT_REG_MEM`
   consumes; the drain forces prior submissions to *execute* (not merely submit) before proceeding.
4. **Incidental GPU forward-progress pump.** The frequent fault-drains were, in effect, pushing
   submissions/labels to completion. Remove the storm and nothing else advances them at the right
   cadence, so the CP stalls. **This is the core design smell: GPU progress must not depend on the
   CPU-fault rate.**

Key data model fact (from prior investigation): the GPU reads **device-local copies** of guest
memory, never guest memory directly — **except `WAIT_REG_MEM`, which reads guest memory directly**
(`CommandProcessor::WaitRegMem`). So the drain is about *command-stream ordering + label/sync-value
visibility*, not about protecting the faulting page's bytes.

## 3. Design principles

- **GPU progress is self-driven**, never a side effect of CPU faults.
- **Fault handling costs O(faulting range)**, not O(whole GPU). The common case (CPU write to a
  streamed page, no readback) must take *no* GPU barrier — only the resource-mutex mutation.
- **Ordering/sync is expressed with GPU primitives** (timeline semaphores / events / fences), not a
  global CPU drain.
- **Every step is independently shippable and validated on the working games** (Greak/Teardown/Metal
  Slug) before the next — this area has 7+ prior failed relaxations, so guardrails matter.

## 4. Phased plan

Each phase lists: goal · approach · risk · validation · exit criterion.

### Phase 0 — Instrumentation (DONE)
The wait watchdog + fault/drain counters exist (committed `ad90964`; counters used ad hoc). Reusable
to measure every later phase. Also add (cheap): **per-page re-fault histogram** and a **drain
cost breakdown** (time in `WaitForIdle` vs `BufferWait` vs `LabelDrain`).

### Phase 1 — Reduce fault *frequency* (MEASURED: dead end, skip)
- **Goal (was):** fewer faults ⇒ fewer drains, without touching the load-bearing drain.
- **Result (measured, `FaultStats` unique-vs-total, GTA III level-load):** the fault storm is
  ~**91-100% unique pages** during active streaming (incremental re-fault ratio ≈1.09; cumulative
  1.4-1.8, inflated only by a little startup churn). So faults are genuine one-shot writes to newly
  streamed asset pages — **not** eager re-protection re-faults. Coalescing re-faults would cut only
  ~10-30%, far short of the ~100× needed. **Phase 1 is not the lever; do not pursue coalescing.**
- **Redirection:** the cost is the *full drain per unique fault*. The 99.6% of faults that are plain
  CPU writes to freshly-streamed pages with no in-flight GPU work need no drain at all; only
  ordering-sensitive pages (WAIT_REG_MEM sync values, in-flight buffers) do. That is **Phase 4**, and
  it requires **Phase 2** first (removing the drain without Phase 2 hangs the CP — verified).

### Phase 2 — Decouple GPU forward-progress from the fault path
- **Goal:** make submissions/labels advance on their own so the fault-drain is no longer the pump.
- **Approach:** a dedicated submission/step driver (or ensure `IncrementDe`/CE-DE handoff and the
  label poll advance the ring without needing a CPU fault). Confirm with the watchdog that with the
  fault path taking *no* drain, the CP still makes progress (labels fire, `WAIT_REG_MEM` resolves)
  under a synthetic "many CPU writes, no GPU stall" load.
- **Risk:** moderate (CE/DE ring management is delicate; see prior `IncrementDe` failures).
- **Validation:** startup no longer hangs when the fault-path drain is stubbed to a no-op *in a
  scratch build*; working games unaffected.
- **Exit:** GPU reaches steady state independent of fault rate.

### Phase 3 — Fence-versioned buffer/const-RAM cache
- **Goal:** answer "is the faulting range still in-flight on the GPU?" precisely, replacing the
  global `WaitForIdle`.
- **Approach:** tag each device buffer / CE const-RAM region with the timeline value of the last
  submission that reads it. On a CPU write fault, record CPU-dirty + the required fence; the next
  `ObtainBuffer` re-uploads only after that fence is signalled (or into a fresh ring slot). No
  whole-GPU wait; at most a wait on one range's fence, usually already satisfied.
- **Risk:** high (correctness-sensitive; ring-buffered CE const RAM is the classic hazard).
- **Validation:** per-range fence waits ≪ old global drains; visual correctness on all games.
- **Exit:** fault path can resolve Continue faults with no GPU wait.

### Phase 4 — Fine-grained fault handler
- **Goal:** delete the per-fault `GraphicsRunSubmissionLock`.
- **Approach:** `HandleFault` external path takes only `ResourceMutex::FaultScope`; classify the
  fault; Continue ⇒ no GPU sync; Download ⇒ defer to the readback worker (which already self-pauses
  when `!submissions_prepaused` — verified); ordering-sensitive `WAIT_REG_MEM` regions handled by
  GPU semaphores from Phases 2–3, not a CPU drain.
- **Risk:** high; this is the payoff step and depends on 2–3 landing first.
- **Validation:** GTA III level-load completes at usable fps; working games unchanged.
- **Exit:** drains/sec drops ~100× on streaming; loading finishes.

## 5. Risks & open questions
- **Headless can't verify visual correctness** — every coherency-touching phase needs an on-machine
  visual pass (the watchdog only proves liveness/perf).
- **CE/DE ring & const-RAM** are the historically fragile parts (multiple prior reverts). Phases 2–3
  are where the real difficulty lives; budget accordingly.
- **`WAIT_REG_MEM` reads guest directly** — confirm which sync values are CPU-written vs GPU-written
  and whether any require CPU→GPU ordering that a GPU semaphore can't express.
- Guest-memory write AVs (`has_any_mapping=0`) and the rare buffer/texture cross-tracker re-entry
  race are *separate* issues (kernel memory-manager / cache coherency) — not fixed by this redesign.

## 6. Appendix — tooling
- `common/waitWatch.h`: per-thread blocking-state registry + `Dump`. Watchdog in `emulator.cpp`
  fires on presented-frame stall. Instrument any new wait with `Kyty::WaitWatch::Scope`.
- Repro: `scratchpad/drive_gta3.ps1` (headless input injection; see `[[gta3-definitive-launch]]`).
- `VkPipelineCache` (`_pipeline_cache.bin`) is orthogonal and already in.
