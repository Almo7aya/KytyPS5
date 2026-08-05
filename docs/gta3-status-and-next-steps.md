# GTA III Definitive Edition — consolidated status and next steps

Written as a handoff. It supersedes nothing but collects what is **settled**, what is **disproven**, what
is **still open**, and — at the maintainer's request — replaces endless probing with **A/B switches** as
the default way to test a hypothesis from here on.

Read this first. The other two docs (`gta3-runtime-fixes.md`, `gta3-black-world-diagnosis.md`,
`gta3-darkness-taa-diagnosis.md`) are chronological and contain claims that were later corrected; the
corrections are consolidated here.

---

## 1. Where the game is

| Area | State |
| --- | --- |
| Boots, reaches gameplay | yes, ~5-20 fps |
| World renders | yes |
| Exposure / brightness | **fixed** — was the missing colour-grading LUT |
| Colour | **fixed** — was the missing pixel-shader ancillary register |
| Street lamps / buildings blinking | **mitigated** — occlusion-query results are no longer trusted |
| Car and character flicker | **OPEN** — the one remaining visible defect |

The remaining symptom, in the maintainer's words: *"for some frames some of the car or the character model
is rendered all over the screen in random places."*

---

## 2. What was fixed, and why it worked

### 2.1 Darkness — the colour-grading LUT (done)

UE builds its combined grading LUT with `RasterizeToVolumeTexture`, a real ES+GS pair.
`ShouldSkipGeShader` dropped it, so the 32³ volume stayed zero and `667515b`'s neutral fallback was
sampled instead — far darker in the shadows than the intended grade.

The pattern needs no geometry stage: the GS is a strict 1:1 passthrough, so ES+GS is semantically an
instanced vertex shader and the ESGS ring round-trip can be **elided** rather than emulated. Implemented as:

- `WriteToSliceAnalysis` — derives the ring-offset → export map from the GS, and the ES store plan from it.
  Verifiable offline: `shader_disasm --writetoslice <es.bin> <gs.bin>`.
- `ir/WriteToSliceLowering` — rewrites each ring store into a single-component export.
- `ExportInfo::component_store` — writes only enabled components, since one hardware export becomes
  several instructions.
- `POS1.z` → `gl_Layer`, with `CapabilityShaderViewportIndexLayerEXT` (SPIR-V 1.3; `CapabilityShaderLayer`
  needs 1.5) and Vulkan 1.2 `shaderOutputLayer` requested at device creation.
- `s3` supplied: it is NGG merged-wave info, and the shader derives EXEC from the ES vertex count in bits
  6:0. Left at zero, EXEC comes out empty and the shader stores nothing — the fix would have looked like a
  no-op.
- User-data window widened for this path: `EsStageRegisters` has no user-SGPR count of its own and
  `SPI_SHADER_PGM_RSRC2_GS.USER_SGPR` describes the geometry half. Measured `rsrc2_user_sgpr=0,
  written_user_sgpr=12`, so it falls back to what the guest actually wrote. The runtime span in
  `TryUseVertexPermutation` had to move in step or nothing would ever hit the permutation cache.

Switch: `KYTY_NO_WRITETOSLICE=1` disables the lowering.

### 2.2 Green cast — the pixel-shader ancillary register (done)

`CombineLUTsPS` builds the LUT from `Neutral = float4(UV * 32/31, LayerIndex / 31, 0)`: red and green from
the interpolated UV, **blue from the slice index**, declared `uint LayerIndex : SV_RenderTargetArrayIndex`
— a *pixel-shader* input. Our GS exports no layer interpolant, so it arrives in the hardware **ancillary
VGPR**, which Kyty never populated. It read zero, every slice was graded as though blue were zero, and
blue died everywhere.

Read out of the dumped shader rather than guessed — its first three instructions are the whole spec:

	v_bfe_u32     v3, v2, 16, 11      ; 11 bits at bit 16 of the ancillary VGPR
	v_cvt_f32_u32 v4, v3
	v_mul_f32     v1, 0x3d042108, v4  ; * 1/31 -> the blue axis

So the render-target array index sits at bits **26:16**, sourced from `gl_Layer` as a fragment input
(reading Layer in a fragment shader is core; only *writing* it from the vertex stage needs a capability).

**Generalisable lesson.** `supported_ps_input_bits` means only that a PS input register is recognised well
enough to *size* the register block — not that anything writes it. A shader reading an
enabled-but-unpopulated register silently gets zero. ANCILLARY sat on that list for the entire
investigation while nothing populated it. The remaining two (`SAMPLE_COVERAGE`, `POS_FIXED_PT`) now report
to stderr on first use.

### 2.3 Static-geometry blinking — occlusion queries (mitigated, not fixed)

Bisection proved it: `KYTY_ZPASS_ALWAYS_VISIBLE` stopped lamps and buildings blinking;
`KYTY_NO_PREDICATION` changed nothing, and a counter showed **zero** draw packets skipped by predication in
any run. So Unreal reads query results on the **CPU** and drops primitives from the next frame's list —
predication is not involved at all.

The queries reported ~47% occluded on an open bridge at night, which is not credible. **Why is still
unknown**, so the results are simply not trusted: every sample takes the monotonic-counter fallback and
reports visible. Cost is over-draw, a frame-rate cost rather than a correctness one; nothing on screen can
be culled (the original Part 5 defect).

Switch: `KYTY_OCCLUSION_QUERIES=1` restores trusting them.

Also fixed on the way: the begin/end dump pairing is now derived from the guest's addressing (the end dump
lands one 64-bit slot above its begin) instead of a bare `armed` flag. Previously any unserviced begin
inverted the pairing for the rest of the recording. Fallbacks went 13306 → 0.

### 2.4 Captures work again (done — this unblocked everything)

F1 capture used to kill the emulator with *"nested exception while resolving a host fault"*. RenderDoc
serialises all mapped memory from its own thread, that walks into write-tracked guest pages, and resolving
one of those faults can fault again — which the filter treated as unconditionally fatal.

Fatal is the right **default**: the fault handler is not re-entrant, it takes the graphics caches' locks and
those have their own recursive-acquisition guards. So with `KYTY_LENIENT_HOST_FAULTS=1` a nested fault is
resolved the only way that is safe from inside the handler — make the page accessible, re-execute, touch no
cache — with depth capped at 4 so real recursion still fails fast.

Cost: that page's GPU-ownership bookkeeping is skipped, so such a capture can hold stale bytes. Acceptable
for diagnosis, not as a default.

---

## 3. The open defect: car and character flicker

### 3.1 Proven root cause: the velocity buffer is never cleared

From `_RenderDoc/kyty_frame951.rdc`, target `47008` (`3360x1892 R16G16B16A16_UNORM`) at pixel (1125, 1360):

	sample_pixel_region -> b = 0.771481   (clearly non-zero)
	pixel_history       -> 0 modifications in the entire frame

No draw in this frame wrote that pixel, yet it holds data. Saved and viewed at full resolution the buffer
contains **five or six overlapping copies of the character**, several cars and several lamp posts at
different positions — while the velocity pass has only **7 draws** and velocity rasterises each object
once. They are successive frames stacked up.

Confirmed from the API side: event 9672, the velocity pass's `vkCmdBeginRendering`, reads `C=Load, DS=Load`,
and the entire frame contains exactly **one** colour clear (event 12115, the separate-translucency surface).

### 3.2 Why this explains every observation

| Observation | Explanation |
| --- | --- |
| Only car and character flicker | only dynamic objects write velocity; static geometry is reprojected from depth |
| Identical standing still or walking | the buffer accumulates either way |
| Unaffected by disabling occlusion culling | visibility was never involved |
| Parts of a model missing, others present | a pixel's motion vector comes from whichever stale silhouette last covered it |
| **Model appears all over the screen** | TAA reprojects using that bad vector and fetches history from wherever it points |
| Renderer drops no draws (`empty=0 no_vertex=0 ge_skip=0` over 240k) | correct — the draws happen; the buffer they write into is dirty |
| R=0/G=0 measured in the velocity buffer | those pixels were simply never written this frame |

### 3.3 Exactly where the clear is lost

Traced with the probe pointed at the velocity surface specifically:

	[surface] velocity base=0x20c11f0000 3360x1892 meta=0x20c51f0000
	[meta] clear addr=0x20c51f0000 code=0x00 flag=0      x456

The clear **is** recorded, 456 times, on a properly registered entry — every one with
`clear_code_valid == false`. Then in `AcquireRenderTargets`:

	if (metadata is Dcc && IsMetaCleared(...)) {
	    if (MetaClearCode(...)) { ...decode... }   // false: no valid code -> nothing happens
	    TouchMeta(..., false);                     // consumed anyway
	}

The clear is recorded, found to have no code, discarded, **and consumed**. Silently.

### 3.4 Three fix attempts, and why each failed

1. **Occlusion-query pairing by address.** Real bug, fixed (13306 fallbacks → 0), but not this bug.
2. **Carry the clear code through the DMA fill** (`BufferCache::FillBuffer` passed `ClearMeta(vaddr)` with
   default arguments). Real gap, and one the docs had already flagged as unplumbed — but the **compute**
   producer is the one at fault here, not the DMA one. Fixed the wrong producer.
3. **Recover the clear code from a shader immediate.** `TryConsumeComputeMetaClear` only recovers the value
   when the shader reads it from a companion read-only buffer — which is how the separate-translucency
   clear works (code `0x40`, so it needs a value). A clear to *zero* needs no companion; the shader writes
   an immediate. The recovery walks back from the store to its register's most recent definition and
   accepts a direct immediate move. **Still flickering**, so either this shader computes the value some
   other way, or the code is not the only thing missing.

### 3.5 What is NOT established

- Whether the velocity clear shader stores a plain immediate at all. Attempt 3 assumed a `v_mov`; if the
  value is computed, the recovery silently returns false. **This is the immediate next thing to check, and
  it should be checked by disassembling that shader, not by another probe.**
- Whether accumulation is the *only* cause. The "all over the screen" symptom is consistent with TAA
  reprojection from stale velocity, but a genuine per-draw transform problem has not been excluded.
- Why the occlusion queries report ~47% occluded.
- Whether the blue cone (see §5) shares a cause.

---

## 4. Methodology change: A/B switches, not probes

**Requested by the maintainer, and correct.** This investigation spent far too many runs on probes, several
of which were badly designed by me:

- a first-sight sampler that reported during loading, before any clear existed;
- an unfiltered trace that saturated on `clear-unregistered` spam (every DMA fill routes through
  `ClearMeta`);
- two probes that needed to be in the *same* build to be correlated, shipped in different ones;
- three separate caps (400, 3000, 96 surfaces) all hit exactly, each time cutting off the one surface of
  interest, which is allocated after dozens of transient ones.

**From here on, prefer an env-gated behaviour change that the maintainer can A/B in one run.** A switch
answers "is this the cause?" directly; a probe only answers "what is the value of X?", which then needs
another round to act on. Probe only when a switch is impossible.

Rules that make this work:
- One switch per hypothesis, default **off**, so the shipped default never changes on an unverified theory.
- Name it after the behaviour, not the theory: `KYTY_FORCE_VELOCITY_CLEAR`, not `KYTY_FIX_FLICKER`.
- Presence-only checks (`getenv != nullptr`) — document that `=0` still enables, because that has bitten.
- Say plainly what each switch costs when it is on (frame rate, correctness elsewhere).

### 4.1 Existing switches

| Variable | Effect | Cost when set |
| --- | --- | --- |
| `KYTY_OCCLUSION_QUERIES=1` | trust occlusion-query results again | lamps/buildings blink |
| `KYTY_NO_PREDICATION=1` | execute every predicated packet | none observed (predication unused) |
| `KYTY_ZPASS_ALWAYS_VISIBLE=1` | legacy alias of the current default | none |
| `KYTY_NO_WRITETOSLICE=1` | disable the LUT lowering | world goes dark again |
| `KYTY_LENIENT_HOST_FAULTS=1` | survive nested faults so F1 capture works | captures may hold stale bytes |
| `KYTY_FORCE_VELOCITY_CLEAR=1` | clear full-res R16G16B16A16_UNORM attachments | **tested: no effect on the flicker** |
| `KYTY_META_CLEAR_ASSUME_ZERO=1` | codeless metadata clear ⇒ clear to zero | **tested: no effect on the flicker** |
| `KYTY_FORCE_BUFFER_UPLOAD=1` | re-upload read-only buffers in full every use | **tested: models got much WORSE** — see §4.1b |
| `KYTY_NO_STALE_UPLOAD=1` | never upload over a GPU-modified read-only range | **tested: no effect** |
| `KYTY_BARRIER_EVERY_DRAW=1` | every draw waits on all prior shader writes | **tested: no effect — but the test was INVALID, see §4.1c** |
| `KYTY_NO_STORAGE_CLAMP=1` | bind the descriptor's nominal size, not the mapped extent | **tested: no effect** |
| `KYTY_BARRIER_AFTER_DISPATCH=1` | barrier after *every* dispatch, not just detected writers | **tested: no effect** |
| `KYTY_FULL_BARRIER_AFTER_DISPATCH=1` | `eAllCommands` / `eMemoryWrite→eMemoryRead` after every dispatch | **tested: no effect** |
| `KYTY_NO_FETCH_COMPONENTS_FIX=1` | revert the cache-hit replay of `resource_fetch_components` | restores the §4.1d bug |
| `KYTY_FETCH_COMPONENTS_IGNORE=1` | always size vertex attributes from `registers_num` | ignores real fetch widths |
| `KYTY_FETCH_COMPONENTS_MAX=1` | always fetch four components per attribute | may read past small attributes |

### 4.1b `KYTY_FORCE_BUFFER_UPLOAD` made it worse — which is the most useful result yet

Forcing read-only buffers to be re-uploaded from guest memory made the models **markedly worse**
(misshapen, wrongly positioned). That is a strong positive signal, not a dead end:

**Guest memory is the stale copy for these buffers, and the GPU is the producer.** Overwriting Kyty's copy
with guest bytes destroys correct data. So Unreal's per-object transforms are written by a GPU pass, not by
the CPU.

That inverts the hypothesis. The bug is not a *missed* CPU write; it is one of:

1. **A stale upload over GPU-produced data.** If the memory tracker ever believes such a range is CPU-dirty,
   the normal `ForEachUploadRange` path overwrites good GPU data with stale guest bytes — for whichever
   objects live in that range, on whichever frames the tracker gets it wrong. That is precisely "some parts
   of some models, some frames". → `KYTY_NO_STALE_UPLOAD`
2. **A missing dependency** between the GPU pass that produces the transforms and the draws that read them,
   so a draw reads partially written data. → `KYTY_BARRIER_EVERY_DRAW`
3. **The storage-buffer clamp** cutting into indexed data, where robust access returns zero. →
   `KYTY_NO_STORAGE_CLAMP`

Test them one at a time. Note 2 and 3 are expected to be slow / wasteful when on — only whether the
misplacement changes matters.

### 4.1c All three were tested: no effect. 1 and 3 are eliminated; 2 was never actually tested

`KYTY_NO_STALE_UPLOAD`, `KYTY_BARRIER_EVERY_DRAW` and `KYTY_NO_STORAGE_CLAMP` all left the flicker
unchanged. Hypotheses 1 (stale upload over GPU data) and 3 (storage clamp) are **eliminated**.

Hypothesis 2 is **not** eliminated, because the test was broken. `KYTY_BARRIER_EVERY_DRAW` emitted its
barrier inside `EmitDrawPrimitives`, which runs with a render pass active. Vulkan permits only subpass
self-dependencies there, so a global buffer dependency at that point is invalid and almost certainly did
nothing. Its negative result carries no information.

What the barrier plumbing actually looks like, having read it:

* `MakeShaderWriteDependency` is **correct and wide** — `srcAccessMask = eShaderWrite`, dst covers
  `eShaderRead/Write`, `eVertexAttributeRead`, `eIndexRead`, `eUniformRead`, transfer and colour-attachment
  access. Destination stages likewise cover `eVertexInput`, `eVertexShader`, `eFragmentShader`, transfer and
  `eColorAttachmentOutput`. (An earlier reading of mine that the destination mask was compute-only was wrong:
  `ShaderWriteBarrier`'s parameter is the *source* stage.)
* The one real gap is that the post-dispatch barrier in `renderCompute.cpp` is **conditional**:

  ```cpp
  bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
  has_storage_writes = std::any_of(program.info.images…, image.written && StorageImage…) || has_storage_writes;
  if (has_storage_writes) { ShaderWriteBarrier(vk_buffer, eComputeShader); }
  ```

  If that detection misses a producing dispatch, **no dependency is emitted at all** and the consuming draw
  is free to read half-written data — for whichever objects that dispatch produced, on whichever frames the
  race lands badly. That is exactly the observed symptom, and it is fully consistent with §4.1b's finding
  that the GPU is the producer.

`KYTY_BARRIER_AFTER_DISPATCH=1` drops the condition. `KYTY_FULL_BARRIER_AFTER_DISPATCH=1` additionally emits
an `eAllCommands`, `eMemoryWrite → eMemoryRead|eMemoryWrite` barrier, which does not depend on Kyty
classifying the write into the `eShaderWrite` bucket at all. Both are placed after `buffer.EndRendering()`,
so unlike `KYTY_BARRIER_EVERY_DRAW` they are legal.

How to read the outcome:

| Result | Conclusion |
| --- | --- |
| `BARRIER_AFTER_DISPATCH` fixes it | write detection (`HasShaderBufferWrites` / the image scan) misses real producers — fix the classifier |
| only `FULL_BARRIER_AFTER_DISPATCH` fixes it | a write escapes the `eShaderWrite` access bucket — widen `MakeShaderWriteDependency`'s src mask |
| neither changes anything | dispatch→draw ordering is fully eliminated; move to vertex/index buffer residency and the embedded vertex-fetch rewrite |

**Both were tested: no effect.** Dispatch→draw ordering is eliminated, and with it every memory-residency and
synchronisation hypothesis. The defect is in how the vertex layout itself is built.

### 4.1d The permutation cache loses `resource_fetch_components` — a real, specific defect

`MakeVertexInputState` in `pipeline/shaders.cpp` chooses each attribute's Vulkan format from the number of
components the shader actually fetches, falling back to the destination register span when that is unknown:

```cpp
auto registers_num   = vs_input_info.resources_dst[index].registers_num;
auto used_components = (vs_input_info.resource_fetch_components[index] > 0
                            ? vs_input_info.resource_fetch_components[index]
                            : registers_num);
GetInputFormat(vs_input_info.resources[index], input_attr[index].format, attr_size, used_components);
```

`resource_fetch_components` has exactly one writer in the whole tree - `RewriteEmbeddedVertexFetches` in
`ShaderRecompiler.cpp:629`, which fills it in through a `const_cast` **as a side effect of recompiling**:

```cpp
mutable_input_info->resource_fetch_components[resource_id] =
    std::max(mutable_input_info->resource_fetch_components[resource_id], inst.input_info.chan + 1u);
```

A permutation cache hit skips the recompile, so nothing ever writes those counts on that path and they stay
at their `= {}` default of zero. The attribute format then comes from `registers_num` instead.

So two draws sharing one shader can build **different vertex layouts**: the draw that compiled the
permutation uses the true fetch widths, and every later draw that hits the cache uses the register span.
Wherever those disagree - a shader fetching two components into a four-register destination - the attribute
format is wrong and the vertex buffer is decoded at the wrong widths. Wrong widths mean positions read from
the wrong bytes, which is geometry landing at arbitrary places.

This matches every property of the bug: it affects only shaders where the two counts differ (so *some*
models), only some attributes within those (so *parts* of a model), it is independent of motion, it is
frame-to-frame unstable because it depends on which draw won the compile, and it is invisible to every
memory-residency and barrier switch tested above.

The fix carries the counts in `ShaderProgramPermutation::vertex_fetch_components` and replays them in
`TryUseVertexPermutation`. It is **on by default**; `KYTY_NO_FETCH_COMPONENTS_FIX=1` restores the old
behaviour for comparison. `KYTY_FETCH_COMPONENTS_IGNORE=1` and `KYTY_FETCH_COMPONENTS_MAX=1` bracket the
field from the other side, pinning every draw to one consistent choice.

| Result | Conclusion |
| --- | --- |
| default build is fixed, `NO_FETCH_COMPONENTS_FIX=1` flickers | confirmed - this was the bug |
| `IGNORE=1` or `MAX=1` also stabilises it | consistency across draws is what matters, not the specific width |
| all four behave identically | the two counts never disagree in practice; re-check `ResolveEmbeddedFetchResource` remapping and index-buffer residency next |

### 4.1e Rounds 3-5: instancing, vertex rate, GPU-modified marking

| Switch | Result |
| --- | --- |
| `KYTY_FETCH_COMPONENTS_*` (4 variants) | no effect |
| `KYTY_FORCE_ONE_INSTANCE`, `KYTY_NO_STICKY_INSTANCES` | no effect |
| `KYTY_ALL_VERTEX_RATE` | **models hugely deformed, covering the screen** |
| `KYTY_MARK_GPU_WRITES` | no effect |
| `KYTY_MARK_GPU_WRITES` + `KYTY_NO_STALE_UPLOAD` | **worse - flicker increased and the HUD blinks** |

**`MarkRegionAsGpuModified` has no caller in the tree** (only its definition at `memoryTracker.cpp:107` and
its declaration). `IsRegionGpuModified` therefore cannot be true for a buffer. Two consequences:

1. **`KYTY_NO_STALE_UPLOAD`'s earlier negative result was invalid** - its guard was `IsRegionGpuModified`, so
   it was a no-op by construction. §4.1c wrongly counted it as an elimination. Corrected here.
2. Marking the ranges does work, but **fixes nothing**, so the stale-upload clobber is *not* the cause. And
   marking plus skipping the upload is actively harmful because `CollectShaderBufferWrites` reports each
   buffer's whole V# footprint (`stride * num_records`), which covers pages the CPU legitimately owns - the
   HUD blinking is CPU-written geometry being starved.

The dead ends are now: TAA/velocity, predication, dropped draws, indirect args, metadata clears, buffer
residency, dispatch→draw ordering, storage clamp, vertex attribute component widths, instance counts, and
GPU-modified tracking. That is every data-supply and synchronisation layer.

**The one positive signal is `KYTY_ALL_VERTEX_RATE`.** Forcing instance-rate buffers to vertex rate deforms
the affected meshes enormously, which proves those buffers are real, are bound to these draws, and carry
transform data. Since `KYTY_FORCE_ONE_INSTANCE` changed nothing, these draws are single-instance, so the
element index is fixed at `firstInstance + 0` and the only per-draw variable is the V# base address and the
buffer's contents.

### Next step: measure, do not switch

A/B switching has now cost five rounds with one bit of information. The remaining hypothesis space is the
recompiled vertex shader's position maths and the per-draw V# base addresses, and both are directly readable
from a capture instead of guessable:

* `get_post_vs_data` on a flickering character/car draw - are the output positions wrong, and wrong how
  (scattered, NaN, one bad component, wrong scale)?
* `get_vertex_inputs` / `get_buffer_data` on the instance-rate binding for that same draw - is the base
  address plausible and the transform sane?
* `disassemble_shader` on that VS to compare against what the recompiler produced.

This needs a capture taken on a frame where the character is **visibly scattered**, not a clean frame.

### 4.1f MEASURED: the exported position's `w` is garbage for a subset of vertices

Capture `_RenderDoc/kyty_frame1207.rdc`, taken on a frame where both characters are visibly scattered.

Character meshes appear in the depth prepass (events 513, 643, 1038, 1048) and the base pass (8466, 10449,
10629, 10647) with matching index counts, so **no draws are being dropped**. Event 643 exports position only
(16-byte stride), which makes it cheap to scan many vertices.

Post-VS `outPerVertex._child0` for event 643, 64 vertices. `z` is 10.0 for every vertex, as expected for
reversed-Z infinite-far (`z_clip = near`). `w` should be `z_view`, a smooth function of position. It is not:

| Vertices | x, y | w |
| --- | --- | --- |
| 18-21, 30-35 | ~(-111.6, 102.6) | -260 |
| 54-56 | ~(-111.7, 102.7) | **+38.1** |
| 60, 62 | ~(-111.0, -268.9) | **+29.4** |
| 61 | ~(-111.1, -268.9) | -267.7 |

**Vertices with near-identical x and y receive wildly different `w`, including opposite signs.** x and y are
transformed correctly, so the matrix and the per-draw transform are fine - `w` alone is wrong, for a subset of
vertices. Vertices 60-62 form a triangle whose `w` spans zero (+29.4, -267.7, +29.4); a triangle straddling
the `w = 0` plane projects to enormous fragments at arbitrary screen positions, and vertices whose `w` lands
on the wrong side are clipped away. Scattered parts and missing parts, one cause.

Two things this rules out, both of which I had previously reported as findings and both of which were wrong:

* **Negative `w` is not the anomaly.** Correctly-rendering world geometry (event 2719) has `w = -5712` and the
  same constant `z = 10.0`. Good and bad geometry share the convention.
* **`TriangleStrip` is not the anomaly.** Both the character draw and the world draw report it.

Also confirmed: `instance_count = 1` on these draws, consistent with `KYTY_FORCE_ONE_INSTANCE` doing nothing,
and character positions are byte-identical between the depth prepass and the base pass, so there is no
per-pass transform divergence.

**Next:** the failure signature - some lanes receiving a value that plausibly belongs to a different vertex -
points at cross-lane handling in the recompiler rather than at any data-supply path. These shaders declare
`gl_SubgroupInvocationID` as an input, and `VWritelaneB32` is handled explicitly at `ShaderRecompiler.cpp:366`
alongside the `ShaderLaneMaskMode` machinery. Disassemble the guest VS for event 643
(`ResourceId::70990`, vs `ResourceId::71082` for the working world draw), find how `w` reaches the POS0 export,
and check every cross-lane step on that path. A capture is no longer needed to make progress on this.

### 4.1g Traced: `w` comes from `v5`; the lane-mask emitter is NOT at fault

Disassembling the character VS for event 643 (`ResourceId::70990`, 2220 lines of SPIR-V):

The position export is a single store at the end of the shader:

	if (_2065) {                                  // _2065 = (exec_lo | exec_hi) != 0
	  float4 _2076 = CompositeConstruct({v9, v2, v6, v5});   // x=v9 y=v2 z=v6 w=v5
	  outPerVertex._child0 = _2076;
	}

So **`w` is whatever `v5` holds at the end of the shader**. `v5` is written repeatedly - lines 156, 182
(`v5 = gl_VertexIndex`), 1089, 1561 (inside a conditional), 1865, 1949, 2053, and last at 2128 - each time as
`Select(exec_active, new_value, old_value)`.

**A hypothesis I formed and then disproved before acting on it.** Every one of those guards reads
`(exec_lo | exec_hi) != 0`, which looks like an "is *any* lane active" test where RDNA requires a per-lane
one. `EmitLaneMaskOperandActiveBool` (`spirvEmitterValues.cpp:612`) does emit that form when
`per_invocation_masks` is set, and the per-lane bit test in the other branch - which reads as inverted, since
graphics always requests `PerInvocation` (`SelectGraphicsLaneMaskMode(64u)` is hardcoded at
`renderDraw.cpp:1065`).

It is not inverted. `EmitPerInvocationMask` (`spirvEmitterAluOps.cpp:5`) stores **1 or 0** into the low dword
and 0 into the high dword: in per-invocation mode a lane mask is a *per-invocation flag*, not a bitmask. For
that representation `(low | high) != 0` is exactly right, and `EmitLogicalBinary` mapping bitwise ops onto
logical ops confirms the design is deliberate. The guards and the position store are correct.

**Where to look next.** Per-invocation masks only hold 0 or 1, so any instruction that treats a lane mask as a
*bitmask* is wrong under this representation - bit counts (`s_bcnt1_i32_b64`), find-first
(`s_ff1_i32_b64`/`s_flbit`), cross-lane reads (`v_readlane`, `v_readfirstlane`), or a mask used as arithmetic
data. Those would silently produce a value from the wrong lane, which is precisely the measured signature of
`w` in §4.1f: vertices at the same x,y receiving another vertex's `w`.

The concrete task: walk `v5`'s definition chain in the event-643 disassembly backwards from line 2128 and find
the first operation on that chain that consumes a mask register as data rather than as a condition. Compare
against the world VS (`ResourceId::71082`), which renders correctly and should either not contain that
operation or reach `w` by a different route.

### 4.1h CORRECTION to 4.1f, and the seeding is cleared

**The §4.1f inference was not sound.** It argued that vertices with near-identical `x` and `y` receiving very
different `w` proved corruption. It does not: `x_clip` and `y_clip` are already projected, so two vertices
sharing them share `x_view`/`y_view` and can legitimately sit at different depths. Differing `w` at the same
projected x,y is ordinary geometry. That table is not evidence of a bug and must not be cited as such.

What is still unexplained is the **spread and the sign mix**: event 643's `w` ranges from -454 to +214, while
correctly-rendering world geometry (2719) is uniformly negative (~-5700). If negative `w` means in front of the
eye here, the positive-`w` vertices are behind it and get clipped. A physically small object cannot span 660
units of depth - but this has *not* been confirmed to be a character mesh. It was selected by index count, not
by checking what it draws on screen. That inference gap is how §4.1f went wrong.

**The v5/v8 ABI seeding is cleared.** With seeding made conditional on liveness (`VgprIsLiveIn`), the default
build and `KYTY_ALWAYS_SEED_VERTEX_INDEX=1` are indistinguishable - both correctly lit, both still scattering.
The reason is visible in the disassembly: `v5` is written at line 1089 and then read at 1090-1091 as
`v5 = v5 + 1`, i.e. index arithmetic. The seeded vertex index *is* genuinely consumed early, and the compiler
then reuses the same register as the `w` accumulator later in the shader. So `v5` is correctly live-in, the
liveness scan classifies it as such, the seed stays, and nothing changed. Seeding was never the defect.

The conditional-seeding change is kept because it is correct on its own terms - it stops seeding registers no
shader reads - but it is not the fix, and `KYTY_NO_INSTANCE_INDEX_SEED` was inconclusive rather than negative:
`v8` is initialised to 0 at line 157 anyway, so skipping the instance seed produces the same 0.

**Do not send more speculative switches.** Rounds 3-7 produced two signals (`KYTY_ALL_VERTEX_RATE` deforms
meshes, `KYTY_FORCE_BUFFER_UPLOAD` worsens them) and no fix, because each round guessed at a mechanism and then
asked for a run. The missing step is cheap and has never been done: **identify which draw actually produces the
scattered pixels.** Use `pixel_history` on a pixel where scattered geometry is visible in the capture's final
image, which names the exact event. Every draw examined so far was chosen by index count and assumed to be a
character. Get the right event first, then read its inputs.

### 4.1i First pixel_history results — a target bound 1044 times and never written

Capture `kyty_frame1207.rdc`, scattering reported as covering the whole screen.

| Query | Result |
| --- | --- |
| `pixel_history(3149, 1680, 200)` | **0 modifications** |
| `pixel_history(3149, 1680, 946)` | **0 modifications** |
| `pixel_history(3282, 1680, 946, event 4643)` | 1 modification, event 3991, pre = post = (0,0,0,0), `pixel_changed: false` |

`ResourceId::3149` is `R10G10B10A2_UNORM` at full resolution and the frame overview attributes **1044 draws**
to it, yet **not one pixel modification is recorded at either of two probed pixels**. A GBuffer attachment bound
across a thousand draws that never receives a write is an anomaly in its own right, and a much stronger lead
than anything in rounds 3-7: candidates are a colour write mask that disables the attachment, every draw failing
the depth or stencil test, or Kyty binding the attachment in the wrong slot so the shader's exports land
elsewhere. A GBuffer channel that is silently never written would leave whatever consumes it reading undefined
data - a plausible route to geometry-looking artefacts spread across the screen.

`ResourceId::3282` receiving a single all-zero, `pixel_changed: false` write at screen centre points the same
way and needs confirming against a pixel known to be covered by solid geometry.

**Next, in order:**

1. Confirm 3149 is genuinely never written - probe several more pixels, and check `get_resource_usage` for it.
   If it truly is never written, find which slot the base-pass PS actually exports to and compare against the
   attachment Kyty binds. `renderDraw.cpp` builds `target_export_mapping` from
   `state.color_info[i].export_mapping`; a wrong mapping here would misroute exports.
2. Only then read the offending draw's inputs. Do not pick draws by index count again - that assumption is what
   invalidated §4.1f.

### 4.1a A/B RESULTS SO FAR — the velocity theory is retired as the *cause*

`KYTY_FORCE_VELOCITY_CLEAR=1` and `KYTY_META_CLEAR_ASSUME_ZERO=1` were both tested: **no difference**. The
flicker is unchanged with the velocity buffer force-cleared every frame.

So although the accumulation in §3.1 is real and proven, it is **not what the maintainer is seeing**. That
retires §3 as the root cause, and two rounds of switching achieved what many rounds of probing did not.

The maintainer's description is the load-bearing evidence now: *"parts of the models look like they are
rendering not in their correct position in some frames and all over the screen."* Recognisable model parts
at wrong positions is **geometry transformed wrongly**. Stale velocity would ghost or smear colour; it
cannot relocate a wheel. So the defect is in what the vertex shader computes, not in post-processing.

That points back at per-draw transform data, which was raised early, dropped on a bad inference
(motion-independence — wrong, the character has an idle animation so his bones update every frame), and
never actually tested. Unreal reads per-object transforms from a storage buffer (GPU Scene: position at
stride 12 plus a per-instance `R32_UINT` id, per Part 4), so the candidates are:

1. **Dirty-range tracking misses a guest CPU write** to that buffer, leaving a stale transform.
   → `KYTY_FORCE_BUFFER_UPLOAD=1` (implemented) re-uploads read-only buffers in full on every use.
2. **The storage-buffer range clamp truncates it.** `NativeStorageBuffer` binds only
   `MappedExtent(address, size)`, and robust buffer access returns **zero** past the bound range — a zero
   transform collapses geometry, a partial one relocates it. Worth a switch that binds the full nominal
   size, or at least logs when a vertex-stage storage buffer is truncated.
3. **Per-draw descriptor staleness** — the permutation cache serving a program with the wrong resource
   snapshot.

### 4.2 Switches to add next, in priority order

1. **`KYTY_FORCE_VELOCITY_CLEAR=1`** — unconditionally clear any full-res `R16G16B16A16_UNORM` colour
   attachment at bind. Crude and title-specific, but **decisive**: if the flicker stops, accumulation is
   confirmed as the entire cause and all remaining work is "clear it correctly". If it does not stop, the
   velocity buffer is not the whole story and §3.5's second bullet becomes the lead. This is the single
   highest-value next step.
2. **`KYTY_META_CLEAR_ASSUME_ZERO=1`** — when a metadata clear is recorded with no decodable code, treat it
   as DCC code `0x00` (clear to black). Tests the swallowed-clear theory generally rather than for one
   surface. Off by default because a DCC *initialise/decompress* is indistinguishable from a clear at that
   point, and treating one as the other is exactly what made the world black in `b38962b`.
3. **`KYTY_META_CLEAR_LOG=1`** — fold the per-surface clear trace behind a switch instead of deleting and
   rewriting it every round. It was rebuilt four times.

---

## 5. Other open items

- **Blue cone in the sky.** Event 12290 in `kyty_frame951`: 786-index triangle strip into the
  separate-translucency buffer, sampling the depth buffer and the volumetric-fog froxel grids
  (`3375`/`3379`, 32³, and `52456`, 210×119). A volumetric light beam belonging to the street lamp, drawn
  **inverted** and centred in the sky. Its transform comes from a uniform buffer, not the vertex stream.
  Only 8 of 786 vertices were inspected, so transform-vs-shading is undecided.
- **Dither/stipple** inside the cone and on the character's clothing. UE dithers volumetric and masked
  sampling and relies on TAA to resolve it, which needs a dither offset that changes **every frame**. Worth
  checking whether the frame index Kyty supplies is static.
- **Metadata keyed by base address.** `m_surface_metas` is keyed on the metadata address alone and
  collisions are real and observed:

	  0x2032ee0000 -> meta=0x2034ee0000   binds=64     clears=0
	  0x2035140000 -> meta=0x2034ee0000   binds=19019  clears=0
	  0x204d640000 -> meta=0x204de40000   binds=15  clears=0
	  0x2052030000 -> meta=0x204de40000   binds=42  clears=1   <- one steals the other's clear

  Not the velocity bug (its metadata address is unique) but a live defect. Key on the range.
- **`kRenderTargetFormats` has no `k16_16_16_16 + kSNorm` entry** while `k16_16` and `k8_8_8_8` do. Any
  title binding that combination hits the `EXIT`.
- **Indirect args read on the CPU.** `DrawIndexIndirectMulti` and `DispatchIndirect` dereference guest
  memory at parse time. Unused by this title (measured), so latent, but wrong for any title that has a GPU
  pass produce them.
- **`HostException` nested-fault leniency** is a diagnostic crutch, not a real fix. A proper fix would flush
  GPU-owned content and drop tracking for the duration of a capture.

---

## 6. Corrections to the older docs

Recorded because each of these was believed and acted upon:

- **`gta3-darkness-taa-diagnosis.md` §9.2 / §11 are disproven.** The "empty R32F at `20235e0000` that no
  draw ever writes" is the **depth buffer** — the TAA binds it twice, as float (depth) and as uint
  (stencil), and it measures depth 0→0.0327 with stencil 0→192. The address mismatch came from comparing
  the texture descriptor against a render-target base under probes capped at 128-256 entries. §11's
  "decisive next step" (aliasing that address to depth) would have been a no-op.
- **§13.2 is also wrong**, and it was mine: I concluded from one dispatch's bindings that the TAA has no
  velocity input at all. A velocity buffer exists and is consumed (read at events 11580 and 12424).
- **Constant `z = 10.0` in post-VS output is correct, not a bug.** UE reversed-Z infinite-far sets
  `z_clip = near` (10uu) and `w_clip = z_view`, so NDC z = near/z_view. This misled Part 4 and then me.
- **Part 9's hand-read WriteToSlice export map had POS0 and PARAM0 swapped.** The derived map puts the
  scale-biased pair in PARAM0 and the raw attribute in POS0, matching UE's `WriteToSliceMainVS`.
- **`504708c`'s diagnosis was wrong.** The analysis rejection was not CFG-insensitivity; a DS store has no
  vdst, so those bits read as register 0 and every `ds_write` appeared to clobber `v0`.
- Claims I made in-session that were wrong: that metadata addresses never collide (based on a truncated
  list), and that motion-independence ruled out skinning data (the character has an idle animation, so his
  bones update every frame regardless).

---

## 7. Practical reference

### Build

	cmake --build _Build/v3 --target kyty_emulator --parallel 9
	cmake --build _Build/v3 --target shader_disasm            # offline shader tooling

`_Build/windows` is stale and points at the **v2** tree — it does not compile these sources. Use `_Build/v3`.

### Run

	_Build\v3\kyty_emulator.exe --printf-direction Silent `
	  --game Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin `
	  2> _gta3_logs\run.stderr.log

`--printf-direction Silent` suppresses `LOGF` entirely, so **anything that must survive the standard repro
has to go to stderr**.

### Run one A/B switch

Every switch is read once through a function-local `static`, so it has to be set **before** launch - setting
it while the emulator is running has no effect. This form sets it, runs, and removes it again, so the
variable never leaks into the next run:

	$env:KYTY_SWITCH_NAME = "1"
	_Build\v3\kyty_emulator.exe --printf-direction Silent `
	  --game Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin
	Remove-Item Env:\KYTY_SWITCH_NAME

`Remove-Item` throws if the variable is already gone, which matters when the run is interrupted before it is
reached. To clear every switch regardless of what an earlier run left behind:

	Get-ChildItem Env:KYTY_* | Remove-Item

Run that before an A/B round to guarantee a clean baseline - a stale `$env:KYTY_*` from a previous test is
the easiest way to mis-attribute a result.

### Capture

	$env:KYTY_LENIENT_HOST_FAULTS = '1'
	# ...run, F1 on a broken frame, quit...
	Remove-Item Env:\KYTY_LENIENT_HOST_FAULTS

Captures land in `_RenderDoc/`. `_RenderDoc/kyty_frame951.rdc` is the one this analysis used and shows the
accumulated velocity buffer.

### Analysing a capture

The RenderDoc MCP server works, and so does the bundled Python module (`C:\Program Files\RenderDoc`,
`import renderdoc`). The techniques that actually produced answers here:

- `save_render_target` then view the PNG — this is what found the accumulation.
- `pixel_history` on a non-zero pixel — zero modifications proves accumulation outright.
- `search_actions "BeginRendering|Clear"` — `C=Load` vs `C=Clear` per pass.
- `disassemble_shader ... search=<term>` — reading the shader settled both the ancillary layout and the
  reversed-Z question. **Reading the shader beat guessing every single time.**

### Offline shader tooling

	_Build\v3\shader_disasm.exe <file.bin>
	_Build\v3\shader_disasm.exe --writetoslice <es.bin> <gs.bin>

Dumps live in `_Shaders/`. `DecodeProgram` gained `return_ends_program` for export shaders, which end with
`s_setpc_b64` rather than `s_endpgm`.
