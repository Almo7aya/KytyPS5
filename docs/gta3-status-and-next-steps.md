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
| `KYTY_NO_STALE_UPLOAD=1` | never upload over a GPU-modified read-only range | may serve genuinely stale data if the tracker is right |
| `KYTY_BARRIER_EVERY_DRAW=1` | every draw waits on all prior shader writes | heavy over-synchronisation, slow |
| `KYTY_NO_STORAGE_CLAMP=1` | bind the descriptor's nominal size, not the mapped extent | may bind unmapped memory; relies on robust access |

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
