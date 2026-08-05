# GTA III Definitive Edition — darkness + flashing diagnosis (current findings)

> **Superseded on the darkness (fixed) and corrected on the flashing.** The darkness was the
> colour-grading LUT, fixed by implementing the WriteToSlice ES+GS lowering plus the pixel-shader
> ancillary register — see `gta3-black-world-diagnosis.md` Parts 10-11. The world now renders with
> correct exposure and colour.
>
> **§9.2 and §11 below are disproven.** There is no empty velocity buffer. Read §13 before acting on
> anything in §9-§11.

Status: **hands-off diagnosis; FPS-crash fixed, darkness root-caused but not yet fixed**. The game
reaches gameplay at 5-10 FPS (was 0.6-1 with the probe-spam crash). Darkness is a two-stage loss
(TAA 0.358→0.097, tonemap 0.097→0.013); the prime root cause is an empty/unaliased motion buffer
`20235e0000` (R32F) that the TAA reads, which should alias the depth buffer at `20235f0000`
(0x10000 higher). See §11 for the actionable next step. Everything below was measured from
`kyty_frame882.rdc` (daytime, captured 12:19 AM), the instrumented run `probe882b.stderr.log`
(01:13 AM), and later live-probe runs `gta3_taa_scene4.err.log`, `gta3_taa_diag2.err.log`
(05 Aug, gameplay).

---

## 1. The user-visible symptoms

- The 3D world renders (DCC-clear fix from `b38962b`-revert work is in place) but the game is
  **very dark**, and models **flash** — present some frames, hidden/black on others.

## 2. Frame-882 measured pass chain (daytime)

Host Vulkan IDs from `kyty_frame882.rdc`:

| Pass | Events | Target | Format | Role |
| --- | --- | --- | --- | --- |
| G-buffer / base | 14400-15604 | 38748, 3150, 3283, 3481 | R11G11B10 + G-buffers | base pass |
| **scene-colour target** | **14572-15405 (52 draws)** | **52619** | **R16G16B16A16** | **writes ~nothing (98.8% empty)** |
| lighting | 15604-16286 | 38748 / 92986 | R11 | lighting |
| half-res chain | 16311-16337 | 53198, 53208 | R16G16B16A16 | mip chain |
| dispatches | 16351, 16374 | — | — | — |
| composite (R11) | 16448-16462 | 53275 | R11G11B10 | — |
| composite (16F) | 16579-16772 | 3429 | R16G16B16A16 | 100% zero |
| scene R11 | 16797 | 3539 | R11G11B10 | bright |
| **TAA dispatch** | **16815** | **3542** | **R16G16B16A16** | **output ≈ history (dark)** |
| bloom chain | 16872-17015 | 48516…48553 | R16G16B16A16 | downsample |
| tonemap | 17089 | 3730 | R8G8B8A8 | near-black |
| display | 17321 | 897 | R8G8B8A8 | — |

### Measured average R channels (frame 882)

| Resource | Format | R avg | Meaning |
| --- | --- | --- | --- |
| `3539` (scene R11) | R11G11B10 | **0.358** | healthy bright daytime scene |
| `3591` (TAA history) | R16G16B16A16 | 0.101 | dark history |
| `3542` (TAA output) | R16G16B16A16 | **0.098** | ≈ history — the new scene is ignored |
| `52619` (TAA input, RGBA16F) | R16G16B16A16 | ~0 | **98.8% empty** |
| `3730` (tonemap out) | R8G8B8A8 | 3.3/255 | near black |

**Key fact:** the TAA reads the bright R11 scene `3539` (0.358) AND an empty RGBA16F `52619`
(98.8% zero), AND the dark history `3591` (0.101), and produces `3542` ≈ 0.098. The bright scene is
not contributing. Then the tonemap multiplies by exposure scalar 1.0 (measured `s30`=1.0 in the
tonemap storage buffer) and still outputs 3.3/255 — so the darkness enters at the TAA stage, not the
tonemap.

## 3. The doc's Part 7 theory is DISPROVEN

The prior doc (`gta3-black-world-diagnosis.md`, Part 7) claimed the tonemapper multiplies by a 1×1
constant texture that reads `(0,0,0,1)` instead of `(1,1,1,1)`, because "a guest CPU write never
reaches the host image".

The instrumented run refutes this:

- `tiny-upload` fires **once per 1×1 texture** (35 total), each with `cpu=1` (the Image constructor
  sets `m_cpu_dirty = !info.data.Empty()`), i.e. every tiny texture IS uploaded from guest backing
  at creation.
- After that they are bound 100K–457K times as `tiny-clean` (never re-uploaded) but the guest
  backing at those addresses is **stable** across the whole run:
  - `0x20047f0000` = `ff000000` (opaque black, always)
  - `0x20047e0000` = `ffffffff` (white, always)
  - `0x2004870000` = `ffffffff` (white, always)
  - `0x2004830000` = `00000000` (tiled, offset-0 read not the pixel)
- `tiny-cpu-write` fired only **7 times in the whole run**, never for the tonemap constants.
- The tonemap's exposure scalar (`s30`, from storage `buffers[1]` word ~542) measures **1.0**.

So the 1×1 constants are correctly uploaded and stable; the exposure is 1.0. The texture cache is
faithfully reproducing guest memory. The darkness is NOT a stale-1×1 problem.

## 4. The real suspect: the TAA dispatch

TAA dispatch 16815 binds (in order):

```
RO [0] 3947  depth     D32S8        3360x1892
RO [1] 4245  1x1        RGBA32F
RO [2] 3947  depth      D32S8        3360x1892
RO [3] 52619 R16G16B16A16 3360x1892  <- 98.8% EMPTY
RO [4] 3539  R11G11B10     3360x1892  <- bright scene (0.358)
RO [5] 3591  R16G16B16A16 3360x1892  <- dark history (0.101)
RW [0] 3542  R16G16B16A16 3360x1892  <- output (0.098)
```

The shader (`sampled_2d[5]` + `storage_2d`) reads all five and writes `3542`. Measured output
`3542` ≈ history `3591` means the new-frame term contributes ~0. The empty `52619` is one of the
inputs; if the shader uses `52619` as the current-frame colour (and `3539` for something else, e.g.
motion vectors / velocity), the blend becomes `history + 0*current` = dark.

Open questions for the TAA:

- Which `sampled_2d[i]` does the shader treat as *current frame colour* vs *history* vs *motion*?
  `52619` is RGBA16F (same format as history/output); `3539` is R11 (the real scene). The format
  mismatch strongly suggests `3539` is *not* the TAA's colour input.
- Where is the TAA blend weight / velocity-rejection constant read from? If the guest computes a
  per-pixel weight from `52619` and it is empty, the weight may be 0.
- `52619` is written by **52 draws** (14572-15405) but ends 98.8% empty. Are those draws:
  (a) genuinely producing nothing in Kyty (culled / shader bug / format issue), or
  (b) the guest's real scene that TAA is supposed to read?

## 5. What the probe instrumentation is (currently in the tree)

`textureCache.cpp`, all tagged `[gta3-probe]`, written to stderr (survives `--printf-direction Silent`):

- `tiny-bind` — every ≤8×8 texture bind in `FindImage`: guest addr/size/fmt/extent, `inserted`,
  `cpu_dirty`, `buffer_mod`, `gpu_mod`, `tile`, and the first 16 guest-backing bytes.
- `tiny-clean` — in `RefreshImage` when a tiny texture is bound but not uploaded (`!cpu_dirty`).
- `tiny-upload` — in `InitializeImage` when a tiny texture IS uploaded, with backing bytes.
- `tiny-cpu-write` — in `InvalidateCpuAliases` when a guest CPU write overlaps a tiny texture.

These are temporary; remove once settled.

## 6. Working-tree state

All uncommitted. Relevant files:

- `src/graphics/host_gpu/renderer/cache/textureCache.cpp` — the four `[gta3-probe]` sites above.
- `src/graphics/guest_gpu/graphicsRun.cpp`, `pm4Handlers.cpp`, `commandProcessor.h` — occlusion
  fix: `PIXEL_PIPE_STAT_DUMP` (0x39) writes a monotonic counter → everything visible.
- `src/graphics/host_gpu/renderer/cache/textureCache.{h,cpp}`, `colorRenderTarget.cpp`,
  `renderDraw.cpp`, `renderCompute.cpp`, `imageInfo.h` — DCC clear-code fix (world renders).
- `src/graphics/shader/shader.cpp` — debug shader dumps / GE-skip logging.

Repro (drive script launches the emulator, dispatches Cross/Start, and can capture F1):

```powershell
powershell -ExecutionPolicy Bypass -File drive_gta3.ps1 -IntroSeconds 55 `
  -PostStartSeconds 55 -Printf Silent -LogTag myrun
# DISPLAY luminance probe lines appear in _gta3_logs\gta3_myrun.err.log every ~180 frames.
# To run the emulator directly:
#   _Build\windows\kyty_emulator.exe --printf-direction Silent `
#     --game Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin `
#     2> _gta3_logs\probe.stderr.log
```

Build: `cmake --build _Build/windows --target kyty_emulator --parallel 9`

## 7. Suggested next experiments (one at a time)

1. **Identify the TAA's current-frame input.** Add a `[gta3-probe]` log in
   `RenderExecutor::DispatchDirect` (`renderCompute.cpp`) for CS dispatches, dumping each
   texture's guest base address + format. Match the TAA dispatch (reads R11 `3539` + RGBA16F
   `52619` + RGBA16F `3591`, writes `3542`) and see which guest address is the "colour" input the
   shader actually blends. Correlate with `52619` vs `3539`.
2. **Check the 52 draws into `52619`.** If `52619` is the guest's real scene, why does it stay
   empty? Probe those draws (shader hash, target format, occlusion state). If they are being
   culled or their shader is skipped, that is the root cause.
3. **Toggle hypothesis cheaply:** if the TAA uses `52619` (empty) as current colour, experiment
   with making the TAA output = `3539` scene (or skipping the empty input) to confirm the scene
   brightens. This is a diagnostic hack only, to confirm the input mapping.

## 8. Known non-issues (avoid re-investigating)

- Tonemap 1×1 constants: correctly uploaded, stable, exposure=1.0. Not the darkness cause.
- Occlusion queries: monotonic-counter fix already makes models appear; they flash and the world is
  dark, but occlusion is no longer culling wrongly.
- The R11 scene `3539` is bright and healthy right up to the TAA dispatch.

## 9. New findings (05 Aug, live probes, gameplay)

### 9.1 The TAA dispatch (live, frame 1-26)

Shader `0x200af90000`, groups `420x237x1`, local `8x8x1`. Sampled/storage bindings:

```
[0] 1x1 fmt56                (scalar, sampled_2d[0])
[1] 20235e0000 R32F 3360x1892 (sampled_2d[1])  <- NEVER written by any draw or dispatch
[2] 1x1 fmt56                (sampled_2d[2])
[3] 203c980000 R11 3360x1892  (sampled_2d[3])  <- bright scene (healthy, gpu_mod=1)
[4] 20023c0000 R8  3360x1892  (sampled_2d[4])
[5] {203e9a/204958} RGBA16F   (sampled_uint_2d[0]) history ping-pong
[6] {203e9a/204958} RGBA16F   (storage_2d[0])  output ping-pong
```

Depth buffer (live) = `20235f0000` D32S8 3360x1892 (see `depth-bind` probe).

### 9.2 The velocity/motion input is empty and never written

`20235e0000` (R32F full-res) is sampled by the TAA and by the pre-TAA full-res dispatch
`0x2005220000`, but **no `rt-bind`, `depth-bind`, or `cs-img ... written=1` ever targets it** in any
probe log. This matches the frame-882 capture, where the TAA's RGBA16F input `52619` was 98.8%
empty/NaN.

Conclusion: the TAA's motion/velocity input is **empty** because the pass that should write it
(the geometry velocity pass) never renders into it in Kyty. With empty motion, the TAA's per-pixel
rejection keeps the blend weight ~0 → output ≈ history (dark), and moving objects ghost/flash.

### 9.3 FORCE-WHITE experiment (still in tree)

`InitializeImage` overwrites every ≤8×8 R8G8B8A8 UNorm upload buffer with `0xFF` before upload.
Confirmed coverage: the tonemap draws sample `0x200fd30000`/`0x200fd40000`/`0x20047f0000` and all
three are FORCE-WHITE'd. If the doc's Part-7 constant theory were right, this would brighten the
game; the constant theory is already refuted by §3, so this is a cheap falsification only.

### 9.4 Probe hygiene / perf notes

- Per-bind probes (`tiny-clean`, `tiny-bind`, `rt-bind`, `taa-bind`, `cs-dispatch`) are rate-limited
  (first N occurrences) to stop the 64 MB stderr spam that collapsed FPS to ~1. Limits: tiny-clean
  128, tiny-bind 256, taa-bind 128, rt-bind 256, cs-dispatch 128.
- In-emulator autopilot (J every 8 s, F1 capture) was removed; the `drive_gta3.ps1` script now
  dispatches Cross (J) and F1 itself. `--autopilot` flag/config plumbing deleted.
- RenderDoc F1 capture during gameplay crashes the emulator with
  `HostException fail-fast: nested exception while resolving a host fault` (a guest page-fault
  handler re-entrancy while RenderDoc snapshots memory). No fresh gameplay capture is available;
  all measurements come from the probes.

### 9.5 Suggested next experiment (decisive)

Bypass the TAA rejection: force the TAA output = scene (e.g. make the TAA's blend weight constant
`0.0099` → `1.0`, or make the shader output the R11 scene directly). If the game brightens and the
flash stops, the empty-velocity theory is confirmed and the real fix is to find/produce the velocity
pass (or emulate motion as zero so TAA uses a stable high blend).

## 10. Experiments run (05 Aug, overnight session)

### 10.1 TAA blend-weight patch (PROVEN INEFFECTIVE)

Patched the TAA shader `0x200af90000` SPIR-V: replaced the hardcoded blend constant
`0x3C23D70A` (0.0099) with `1.0` (`0x3F800000`). The patch fired ("replaced 1 instances", gated on
`KYTY_TAA_BLEND_PATCH`) but the DISPLAY luminance was **identical** to the control run
(world ~8/255, loading-screen 68-77). Conclusion: forcing TAA output = scene does not brighten the
display, so either the TAA output isn't the display's source, or a downstream stage still crushes.

### 10.2 Tonemap constant patches (PROVEN INEFFECTIVE)

- Patched tonemap PS `0x200ccf0000` SPIR-V: replaced `0.3EA60A0D` (0.31395) → 1.0: "replaced 0".
- Patched all small float literals (0.25/0.5/0.31395) → 1.0: "replaced 1", display unchanged.

The tonemap's darkening multipliers live in a **guest SRT buffer** (offset 20972096 dump):
`[0]=0.5 0.5 0.5 0.5`, `[1]=1.0 1.0011`, `[4]=0.25`. These are correct guest data, so the tonemap
faithfully applies them — the real fix is upstream (the TAA output being too dark).

### 10.3 FORCE-WHITE experiment (PROVEN INEFFECTIVE, now removed)

Forcing every ≤8×8 R8G8B8A8 UNorm upload to white (covering the tonemap's sampled 1×1 constants
`0x200fd30000`/`0x200fd40000`/`0x20047f0000`) did NOT change the DISPLAY luminance. Combined with
§3 (constants upload correctly, exposure=1.0), the 1×1-constant theory is fully disproven.

### 10.4 Definitive frame-882 numbers (verified with correct decodes)

- Scene R11 `3539` = **0.358** (bright, Rmax 0.56) — healthy
- G-buffer R11 `38748` = 0.293, lighting `92986` = 230/255 — bright
- **TAA out `3542` = 0.097** ← scene 0.358 → 0.097 (3.7x loss at the TAA)
- **Tonemap out `3730` = 3.3/255** ← 0.097 → 0.013 (7.5x crush at the tonemap)
- Display = 3.3/255
- Composite16F `3429` = all zero (the composite/billboard pass produces nothing)
- TAA history `3591` = 0.101, LUT `3738` = healthy ramp (not empty)
- TAA motion input `52619` = NaN/garbage (98.8% empty)

The darkening is TWO-STAGE: TAA (3.7x) then tonemap (7.5x). Both stages are faithful to guest data
(empty velocity, dark TAA out → dark tonemap). The **empty velocity buffer** (never written by any
draw/dispatch — `20235e0000` R32F) is the prime root-cause candidate.

### 10.5 New measurements / tooling (working, kept)

- `Presenter::Present` DISPLAY luminance probe (staging-buffer readback, every 180 frames,
  `[gta3-probe] DISPLAY lum=...`). Reliable; log size now ~260 KB vs 64 MB before throttling.
- Probe rate-limits: tiny-clean 128, tiny-bind 256, taa-bind 128, rt-bind 256, cs-dispatch 128,
  tonemap-draw 256. This restored FPS from ~0.6-1 (probe spam) to ~5-10 (gameplay).
- In-emulator autopilot removed; `drive_gta3.ps1` dispatches J/F1, supports `-Env` hashtable and
  `-CaptureEvery N` (F1 captures still crash the emulator mid-gameplay).
- `depth-bind`, stage-luminance (`ProbeImageLuminance` from present), and the 4096-cap rt-bind
  probes were added then REMOVED after they proved crash-prone (scheduler re-entry from the
  present thread).

### 10.6 Known instability

The game crashes ~50% of runs with `HostException fail-fast: nested exception while resolving a
host fault` (a guest page-fault handler re-entrancy). This predates the probes and is independent
of the darkness investigation; RenderDoc F1 capture triggers it reliably mid-gameplay.

## 11. Velocity-buffer address hypothesis (05 Aug, overnight)

The TAA dispatch (live) samples an R32F texture at `20235e0000` as its motion input, and the
depth buffer is at `20235f0000` (D32S8) — exactly **0x10000 apart**. In the frame-882 capture the
TAA sampled the **depth texture twice** (RO[0] and RO[2], both D32S8 `3947`). This strongly suggests
the TAA's R32F "velocity" input is actually the **depth buffer** (depth sampled as R32F), and the
live descriptor decodes it to `20235e0000` instead of `20235f0000` — a 0x10000 (one-page) base
offset error in the depth-texture descriptor decode.

Supporting evidence:
- `20235e0000` (R32F) is sampled by the pre-TAA dispatch `0x2005220000` and the TAA, but **never
  written by any draw or dispatch** in any probe log (`rt-bind`, `depth-bind`, `cs-img`).
- `ClearImageFromBuffer(0x20235e0000)` returns **ok=0** — the texture cache has no clearable image
  there; it is not a real registered allocation.
- The depth buffer `20235f0000` (D32S8) IS a registered, drawn-to target.
- The capture's `52619` (the TAA motion input) was NaN/garbage = reading uninitialized memory.

Next step (decisive): make the TAA's R32F input decode to the depth base `20235f0000` (fix the
0x10000 offset), OR alias `20235e0000` to the depth allocation, and re-measure DISPLAY luminance.
If the TAA then sees real depth, its motion/rejection logic gets valid data and the output should
brighten toward the scene (0.358) instead of staying at history (0.097).

Also noted: patching the tonemap PS `0x200ccf0000` literal `0.5` (0x3F000000, used only in UV
coord remaps `Fma(v,-0.5,0.5)`) to 1.0 had no meaningful display effect. The tonemap's darkening
constants (0.5/0.25 in the SRT buffer) are faithful guest data; the crush is downstream of the dark
TAA output.

## 12. Command-buffer-dump findings (06 Aug)

Ran with `--command-buffer-dump true --command-buffer-dump-folder _Buffers_gta3`. Each frame dumps
per-buffer PM4 streams (`<frame>_<seq>_buffer_{d,a,ci,cc}.log`), which resolve indirect register
blocks (`IT_SET_CONTEXT_REG_INDIRECT` etc.) into `offset = ..., value = ...` pairs.

### 12.1 Frame 1400 (dark gameplay) render-pass structure

| Buffer | draws | color targets | notes |
| --- | --- | --- | --- |
| 16408 | 10 | CB0=0x2013500000 | small pass, mask=0 |
| 16409 | 17 | CB0..3=G-buffer | G-buffer prepass, mask varies |
| 16413..16419 | 43 each | none | depth-only / occlusion prepasses |
| 16420 | 167 | none | occlusion-query pass: **334 PIXEL_PIPE_STAT_DUMP (event 0x39)**, pairs 8B apart |
| 16423 | 52 | CB0..3 = 0x2035140000 / 0x2028630000 / 0x202a650000 / 0x202c670000 | **CB_TARGET_MASK=0** (color writes masked off) |
| 16427..16433 | 45 each | same CB0..3 | **CB_TARGET_MASK=0xffffff** (color writes on) |
| 16434 | 119 | CB0=0x8fc0000000 (display), CB1..3=composite inputs | final composite, mask 0x0f |

Key facts:
- The scene has **real geometry** (draws with up to 10k+ vertices), valid color+depth targets, and
  a color-writing base pass (16427, CB_TARGET_MASK=0xffffff). The game intends a full scene render.
- The G-buffer `0x2028630000` (A2B10G10R10) is bound by many passes; the base pass writes it.
- The final composite renders to the display `0x8fc0000000` and reads the composite inputs.
- Predication: ~27% of predication checks result in `skip=1` (draws skipped). Forcing all
  predicated draws (`KYTY_NO_PREDICATION`) did **not** change DISPLAY luminance — the darkness is
  not from predication culling.
- Only 16 predication addresses flip between skip/no-skip across the sample — the flashing-model
  candidates.

### 12.2 Why the world is dark (candidate root causes)

1. The G-buffer base pass (16427) has CB_TARGET_MASK=0xffffff, so it *should* write color. If the
   G-buffer is genuinely black despite this, the fragment shader output is wrong (shader
   miscompile / wrong export mapping / wrong color-slot binding).
2. The depth-only passes (16423, mask=0) are correctly masked off; they are not the bug.
3. The composite (16434) renders to the display; if its inputs (G-buffer / scene color) are black,
   the output is dark. The G-buffer content must be verified with a correct readback (the earlier
   `nz=0/24990` readback for A2B10G10R10/layout-5 images was never validated against a known-good
   image and may itself be the artifact).

### 12.3 Flashing models (candidate root cause)

The occlusion queries (event 0x39) write a monotonic 64-bit counter to GPU addresses
(`0x1181143de0`-class, <2^40, CPU-mapped). Predication reads main-RAM flags (`0x20e8b47830`-class)
that the game derives from the query results. 16 addresses flip between visible/hidden. If the
game's query-result readback is stale (reads the GPU counter before the CPU memcpy is visible, or
the pair's begin/end are written across frames inconsistently), models flash. Next: verify the
query result reaches the predication flag deterministically per frame.

## 13. The velocity-buffer theory is disproven (05 Aug, from `kyty_frame1659.rdc`)

Measured directly out of the capture at the TAA dispatch (event 17081). No emulator run needed — the
TAA is the guest's own shader and the darkness work did not touch it, so an older capture is still
authoritative about its inputs.

### 13.1 The "empty R32F velocity buffer" is the depth buffer, and it is populated

| Binding | Resource | Format | Size |
| --- | --- | --- | --- |
| `sampled_2d[0]` | 4243 | — | 1×1 |
| **`sampled_2d[1]`** | **3945** | **D32S8** | **3360×1892** |
| `sampled_2d[2]` | 685 | — | 1×1 |
| `sampled_2d[3]` | 3536 | R11G11B10 | 3360×1892 |
| `sampled_2d[4]` | 3588 | R16G16B16A16 | 3360×1892 |
| **`sampled_uint_2d[0]`** | **3945** | **D32S8** (same resource) | 3360×1892 |
| `storage_2d[0]` | 3539 | R16G16B16A16 | 3360×1892 |

`3945` measures depth 0→0.0327 and stencil 0→192, i.e. **real content**. It is bound twice: as float
(depth) and as uint (stencil).

So what §9.2 called "an R32F full-res texture at `20235e0000` that no draw or dispatch ever writes" is
the depth buffer. The address mismatch in §11 was an artefact of comparing two different sources — the
*texture descriptor* Kyty decoded for the sampled depth against the *render-target* base reported by
`rt-bind`/`depth-bind` — under probes rate-limited to 128-256 entries in a frame with thousands of
binds. "Never written" was never established; it was the cap.

**§11's "decisive next step" — aliasing `20235e0000` to the depth allocation — would have been a no-op
at best. Do not implement it.**

### 13.2 This TAA has no velocity input at all, so missing motion vectors cannot be the cause

Five sampled inputs: two 1×1, depth (twice), one full-res R11G11B10, one full-res RGBA16F, plus one
RGBA16F storage output. **None is a two-channel motion/velocity texture.** The guest compiled this TAA
variant without one, so no amount of emulator work can make it consume velocity. The framing of "the
velocity pass is missing" is retired.

### 13.3 What that leaves: stencil is how this TAA finds moving geometry

The TAA samples **stencil**. In UE that is how responsive-AA is marked: geometry tagged
`STENCIL_TEMPORAL_RESPONSIVE_AA` gets a much higher blend weight so it does not ghost, and it is the
mechanism a velocity-less TAA configuration uses for moving and dithered-opacity geometry. That
connects directly to Part 4's still-unexplained stipple on the character's shorts and legs, which is
dithered opacity TAA never resolved.

If the stencil the TAA samples does not match what the depth pass wrote, moving and dithered geometry
gets the wrong blend weight while static geometry is unaffected — a motion-dependent flicker, which
matches "flash more if they are moving" in a way TAA ghosting does not. Ghosting smears; it does not
blink.

Ruled out already by reading the code rather than guessing:

- `TextureCache::SwapDepthAlias` (`146f848`) refuses any surface where either side `HasStencil()`, so
  depth/colour aliasing cannot be dropping the stencil plane.
- `ResolveDepthOverlap` does not recreate on a Sampled↔Storage change between two colour bindings, so
  the history/output ping-pong is not being destroyed and rebuilt by the cache.

### 13.4 Do not trust these resource identifications further than stated

Which of `3536` / `3588` is the colour input versus the history is **not** established. Their maxima do
not line up with a convex blend (scene 0.108, history 32.25, output 98.0). The likeliest explanation is
that the maxima come from a small sodium street-lamp region while the bulk of the frame is ~0.0025
neutral grey — but it is unconfirmed. Identify these from the shader's own use of each descriptor, not
from brightness.

### 13.5 The measurement that would settle it

Needs a run, or a capture taken while the flashing is visible:

1. **Two consecutive frames**, to see the `{3588, 3539}` ping-pong swap and confirm both sides survive
   the frame boundary holding what the previous frame wrote.
2. **The TAA's stencil reads versus the depth pass's stencil writes**, per §13.3.

### 13.6 A trap worth recording, because it cost several rounds

Kyty's `supported_ps_input_bits` says only that a pixel-shader input register is recognized well enough
to *size* the register block — not that anything is written into it. A shader reading an
enabled-but-unpopulated register silently gets zero. ANCILLARY sat on that list for the whole
investigation while nothing populated it, which is exactly why the colour-grading LUT lost its blue
axis. Kyty now reports the remaining ones (`SAMPLE_COVERAGE`, `POS_FIXED_PT`) to stderr on first use
rather than failing silently.
