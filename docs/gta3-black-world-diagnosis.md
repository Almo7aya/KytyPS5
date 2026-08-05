# GTA III Definitive Edition — black 3D world diagnosis

Investigation of the remaining "world 3D renders black, HUD/minimap fine" defect on
`fix/gta3-runtime-fixes` (HEAD `b38962b`). This continues `docs/gta3-runtime-fixes.md`, which covers
the six earlier blockers, and supersedes the assumption in commit `667515b` that the colour-grading
LUT was the cause of the black world.

Reproducer:

```powershell
_Build\windows\kyty_emulator.exe `
  --printf-direction Silent `
  --game Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin
```

Primary evidence: `_RenderDoc/kyty_frame2404.rdc` (captured 17:25, i.e. after `b38962b`), plus the
older `kyty_frame2188.rdc` / `kyty_frame2254.rdc`.

## Symptom

Gameplay is reached. The HUD, wanted stars, cash, health and the circular minimap all render
correctly. The 3D world is black except for a soft orange/red plume (a fire/particle effect) and a
few very bright specks.

## Frame structure (frame 2404)

`analyze_render_passes` on the capture gives this chain. Resource IDs are host Vulkan IDs from that
capture.

| Pass | Events | Target | Format | Draws | Role |
| --- | --- | --- | --- | --- | --- |
| 0 | 689–1299 | 92766 + 3197/3330/3526/3479/4258 | R11G11B10 + G-buffers | 35 | base pass |
| 1 | 1328 | 3325 | R8 | 1 | mask / AO |
| 2 | 1373–1648 | 92766 | R11G11B10 | 15 | deferred lighting |
| 3 | 1729 | 3637 | R16G16B16A16 | 1 | — |
| 4 | 1754–1938 | 92766 | R11G11B10 | 8 | translucency / fog |
| 5 | 1997–2071 | **3475** | R16G16B16A16 | 4 | **separate translucency** |
| 6 | 2096, 2140 | 3585 | R11G11B10 | 2 | **separate-translucency composite** |
| — | 2113 | 3588 | R16G16B16A16 | dispatch | TAA (compute) |
| 7–23 | 2170–2621 | 92802…102754 | — | 1 each | downsample + bloom chain |
| 24 | 2713 | 3923 | B8G8R8A8 | 1 | tonemap |
| 25 | 2803–3087 | 950 | B8G8R8A8_SRGB | 18 | HUD / UI |

## Where the world is lost

Measured with `sample_pixel_region` / `read_texture_pixels`:

| Resource | Event | Contents |
| --- | --- | --- |
| 3197 (G-buffer normals) | — | intact world geometry (see `_gta3_logs/renderdoc_frame2254/3144_gbuffer_0.png`) |
| 92766 (scene colour) after base pass | 1299 | mean 0.0026, max 0.0144 — populated |
| 92766 after lighting | 1648 | mean 0.0046, max 0.0605 — populated |
| 92766 after translucency | 1938 | mean 0.0040, max 0.0566 — populated |
| 92766 before composite | 2071 | mean 0.0040, max 0.0566 — **still populated** |
| **3475 (separate translucency)** | 2071 | **exactly `(0,0,0,0)` including alpha** |
| 3585 (composite output) | 2096 / 2140 | **all zero** |
| 3588 (TAA output) | 2713 | all zero |
| 3923 (tonemap output) | 2713 | all zero |

So the scene survives lighting and translucency intact, and dies precisely at the
separate-translucency composite (pass 6).

`pixel_history` on 3585 at (1680, 946) shows both composite draws with `passed = true` and
`pixel_changed = false`, `post = (0,0,0)`. The fullscreen triangle is fine — `get_post_vs_data` at
event 2096 returns the standard `(1,-1) (-3,-1) (1,3)` clip-space triangle. The pixel shader runs and
outputs zero.

## The composite shader

Pixel shader `ResourceId::3596` at event 2096, decompiled from host SPIR-V. Bound
`sampled_2d[0..2]` are, in array order, 92766 (scene colour), 705 (a 1×1 texture reading
`(1,1,1,1)`), and 3475 (separate translucency).

The tail of the shader is:

```
v4 = A.x * B.x ; v5 = A.y * B.y ; v6 = A.z * B.z
v0 = Fma(C.w, v4, C.x)
v1 = Fma(C.w, v5, C.y)
v2 = Fma(C.w, v6, C.z)
out_mrt_0 = pack(v0, v1, v2, ...)
```

That is Unreal's separate-translucency composite:

```
SceneColor = SceneColor * SeparateTranslucency.a + SeparateTranslucency.rgb
```

Unreal clears the separate-translucency target to `(0, 0, 0, 1)` precisely so that, where no
translucency was drawn, alpha 1 passes the scene through unchanged.

Kyty leaves that target at `(0, 0, 0, 0)`. Alpha is 0, so the composite evaluates to

```
SceneColor * 0 + 0 = 0
```

for every pixel, which zeroes the entire world.

### Why this matches the screenshot exactly

- The world is multiplied by alpha 0, so all opaque geometry disappears.
- Anything drawn *into* the separate-translucency buffer contributes additively through
  `+ SeparateTranslucency.rgb`, so it survives. That is the orange fire plume.
- The HUD and minimap are drawn much later, into display target 950 (pass 25), so they are
  unaffected.

This also explains why the earlier colour-grading-LUT work did not fix the world: the LUT is fine.
Verified in this capture — LUT 3931 is populated (`0, 0.0029, 0.0078, 0.0156, …` along the red axis)
and the exposure buffer 4451 reads `1.2656`. Both inputs to the tonemapper are healthy; its scene
input 3588 was already zero.

## Root cause: the fast clear of 3475 uses the wrong value

`get_action(1997)` reports:

```
vkCmdBeginRendering(C=Clear, DS=Load)   outputs: ResourceId::3475
```

So Kyty **is** issuing `loadOp = CLEAR` for the separate-translucency attachment. The clear fires but
clears to `(0,0,0,0)` instead of `(0,0,0,1)`.

The clear path (added in `b38962b`) is:

1. `ResolveRenderColorTarget` (`colorRenderTarget.cpp:322`) — when
   `rt.info.dcc_compression_enable && rt.dcc_addr.addr != 0`, decode `CB_COLOR#_CLEAR_WORD0/1` into
   `r.color_clear_value` and record `desc.info.metadata = {rt.dcc_addr.addr, kind = Dcc}`.
2. `TextureCache::FindRenderTarget` (`textureCache.cpp:1446`) registers that address in
   `m_surface_metas`.
3. A guest metadata clear-fill marks it cleared — either a compute dword-fill
   (`TryConsumeComputeMetaClear`, `renderCompute.cpp:70`) or a CP DMA fill
   (`BufferCache::Fill`, `bufferCache.cpp:751`), both via `TextureCache::ClearMeta`.
4. `AcquireRenderTargets` (`renderDraw.cpp:547-553`) turns a cleared metadata state into
   `attachment.is_clear`, with `attachment.clear_value = target.color_clear_value.uint32`.

Steps 1–4 demonstrably run for 3475 (hence `C=Clear`). The clear *value* is what is wrong.

Confirmed facts about the register state, from `_gta3_logs/nullwrite_diag_20260804_093330.stdout.log`:

```
RenderTarget: temporary: ignoring PS5 color metadata fast_clear=false dcc=true \
    cmask=0x0000000000000000 dcc_addr=0x0000002023080000
```

So the game uses **DCC**, not CMASK (`cmask.addr == 0`, `cmask_fast_clear_enable == false`), and
`dcc_compression_enable == true` — the `b38962b` path is the correct mechanism, and it is active.

Ruled out during the investigation:

- **Clear-value plumbing.** `color_clear_value.uint32` → `RenderAttachment::clear_value` →
  `clearValue.color.uint32` (`context.cpp:276`) is bit-preserving through the `vk::ClearColorValue`
  union, so float bits survive.
- **`DecodeColorClear` for `R16G16B16A16_SFLOAT`** (`colorRenderTarget.cpp:41-54`) is correct.
  Hand-checked: `ColorClearF16` reads only bits 0–15 of its argument, and for a `(0,0,0,1)` pixel
  (`clear_word0 = 0x00000000`, `clear_word1 = 0x3C000000`) it yields exactly `(0,0,0,1)`.
- **Missing PM4 dispatch entry.** `g_hw_ctx_func` has no entry at `CB_COLOR#_CLEAR_WORD0/1`
  (`pm4Dispatch.cpp:88-89` only registers `CB_COLOR0_BASE` and `CB_COLOR0_INFO` per slot), but
  `pm4Handlers.cpp:2763-2775` falls back to `g_hw_ctx_indirect_func` per register when the table
  entry is null, and those *are* registered for `CLEAR_WORD0/1`
  (`pm4Handlers.cpp:3195-3213`). Direct register bursts are therefore not dropped.

### `b38962b` looks like the regression

An A/B across the two captures points at the DCC fast-clear commit itself:

| Capture | Time | Build | Tonemapper scene input |
| --- | --- | --- | --- |
| `kyty_frame2188.rdc` | 16:13 | `bc18722` (before `b38962b`) | resource 3484: mean 0.113, max 0.5 — **populated** |
| `kyty_frame2404.rdc` | 17:25 | `b38962b` | resource 3588: **exactly 0** |

`b38962b` was committed at 16:32, between the two captures. Same pipeline position, same game state,
opposite result.

That is consistent with the following mechanism. Before `b38962b` Kyty never cleared colour
attachments, so the separate-translucency attachment was `loadOp = LOAD` and its host image was
populated from the guest backing — the guest's own clear of that surface memory (to `(0,0,0,1)`)
reached the image through the normal texture-upload path, so alpha was 1 and the composite passed the
scene through. `b38962b` then added `loadOp = CLEAR` using `CB_COLOR#_CLEAR_WORD0/1`, which for this
target decode to `(0,0,0,0)`, and that clear **overwrites** the correct uploaded contents at
render-pass begin.

In other words the new clear is very likely both *spurious* (triggered by any write to the DCC
allocation, including a DCC init/decompress — `TryConsumeComputeMetaClear` and `BufferCache::Fill`
never inspect the value written) and *wrong-valued* (the guest never programmed a fast-clear colour
for this target, so the words are zero).

### Confirmed by experiment

A build with the metadata colour clear suppressed (the attachment left as `loadOp = LOAD`, everything
else unchanged) was run against the same game. **The 3D world renders**: road surface, the Callahan
Bridge structure, street lights, vehicle tail lights, sky and the location caption are all present.

That confirms `b38962b` as the regression, and confirms the mechanism above: the guest's own clear of
the separate-translucency surface already reaches the host image through the normal texture-upload
path, and `b38962b`'s attachment clear was overwriting it with an unprogrammed `(0,0,0,0)`.

## Resolution

`b38962b` is reverted (`colorRenderTarget.cpp`, `renderDraw.cpp`, `textureCache.cpp` restored to their
`bc18722` state). Colour render targets are again loaded rather than cleared, and DCC metadata
allocations are no longer registered in `m_surface_metas`, so guest writes to them execute normally
instead of being swallowed as fast clears.

If PS5 colour fast clears are implemented properly later, the following must hold — `b38962b` violated
all three:

1. **Do not treat every write to a DCC allocation as a fast clear.** `TryConsumeComputeMetaClear`
   (`renderCompute.cpp:64-75`) and `BufferCache::FillBuffer` (`bufferCache.cpp:751`) never inspect the
   value written, so a DCC initialise or decompress is indistinguishable from a clear — and the write
   is then *skipped* as well as misinterpreted.
2. **Do not clear with a colour the guest never programmed.** `CB_COLOR#_CLEAR_WORD0/1` are zero for
   this target; clearing to `(0,0,0,0)` is strictly worse than loading.
3. **Do not key fast-clear state on a bare address.** `desc.info.metadata.range` was stored with
   **size 0** (`colorRenderTarget.cpp:326`) and `m_surface_metas` is keyed by address alone, so
   aliased or pooled metadata allocations can cross-trigger between render targets.

A useful diagnostic note for future work: `--printf-direction Silent` sets the log sink to
`Direction::Silent` (`log.cpp:152-158`), which suppresses `LOGF` entirely, and
`graphics_debug_dump_enabled()` additionally requires a non-Silent direction (`debug.cpp:59-62`). Any
probe meant to survive the standard repro must write to stderr, or the run needs
`--printf-direction Console`/`File`.

## Building this tree

`_Build/v3` is a build directory configured for **this** tree. Note that `_Build/windows` in this
checkout was copied from `KytyPS5_v2` and its CMake cache still points there, so it does **not**
compile these sources — builds run there silently produce a binary from the v2 tree.

```powershell
cmake -S . -B _Build/v3 -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.11.1/msvc2022_64"
cmake --build _Build/v3 --target kyty_emulator --parallel
```

The temporary stderr probes and the `KYTY_NO_META_COLOR_CLEAR` A/B switch used to reach the
conclusion above have been removed along with the revert. If they are needed again, the probe points
that mattered were: the decoded clear value per colour target in `ResolveRenderColorTarget`, the value
actually applied where `attachment.is_clear` is set in `AcquireRenderTargets`, and the address plus
filled value at each `TextureCache::ClearMeta` hit.

## Part 2 — the revert is not sufficient (daylight frame 9321)

**Correction.** An earlier revision of this document argued the leftover darkness was probably not a
defect, reasoning from the night frame that auto-exposure looked healthy. That was wrong. A daylight
capture (`_RenderDoc/kyty_frame9321.rdc`, in-game 09:01) shows an unmistakable second defect: a black
sky above a flat blown-out grey ground, split by a hard horizontal seam, with heavy blocky dithering.

### It is the same failure, still

Pass structure in frame 9321 (783 shadow draws now that it is daytime):

| Pass | Events | Target | Role |
| --- | --- | --- | --- |
| 0 | 21–10370 | depth only | shadow maps, 783 draws |
| 1 | 10801–11463 | 32099 + G-buffers | base pass, 38 draws |
| 3 | 11540–11663 | 32099 | lighting, 6 draws |
| 7 | 12237–12410 | 32099 | translucency, 7 draws |
| 8 | 12481–12553 | **3419** | **separate translucency**, 4 draws |
| 9 | 12578–12622 | 3530 | **separate-translucency composite**, 2 draws |
| 17 | 12878 | 3722 | tonemap |
| 18 | 12930–13126 | 3808 | HUD |

Measurements:

- Scene colour `32099` after lighting is **healthy**: mean `(0.040, 0.070, 0.107)`, max b `0.3125` —
  a plausible daylight HDR scene, blue-dominant. The world is intact right up to the composite, exactly
  as in the night case.
- Separate translucency `3419` has **alpha = 0 everywhere**, with RGB ≈ `(0.0023, 0.0023, 0.0022)` in
  the sky region (y = 300) and RGB ≈ `(0.206, 0.217, 0.228)` in the ground region (y = 1400).

Feed that through the composite `out = SceneColor * A + C` with `A = 0`:

- sky → `0.0023` ⇒ black upper half
- ground → `0.21` ⇒ flat grey lower half

So the final image is *nothing but* the separate-translucency RGB, and the horizontal seam is simply
the silhouette of whatever was drawn into that buffer. The world is still being multiplied out.

### The content is stale, not blended down

`pixel_history` on `3419` at (1600, 1400) reports exactly **one** modification in the whole frame —
event 12553, which *failed* the depth test. Its `pre_value` is already
`(0.206, 0.217, 0.228, a = 0.0)`. So that content was present at **attachment load**, before any draw
in the pass. It is leftover from an earlier frame, not the result of alpha blending.

### Conclusion: the clear is genuinely required

Both behaviours tried so far are wrong:

- `b38962b` cleared the attachment to the unprogrammed `CB_COLOR#_CLEAR_WORD` value `(0,0,0,0)`
  ⇒ alpha 0 ⇒ world black.
- The revert loads the attachment ⇒ stale content, alpha still 0 ⇒ world replaced by flat grey.

Unreal needs this attachment to start at `(0,0,0,1)`. The revert remains the better of the two (it at
least renders a plausible night scene, and it removes a clear that fires on unvalidated metadata
writes), but it is not the fix. The missing piece is unchanged and now sharply defined: **where does
the guest's `(0,0,0,1)` clear value go?**

### Ruled out in this frame

- **A raw compute fill of the surface.** Dispatch 12437 sits immediately before the separate-translucency
  pass and *is* a single-dword buffer fill (it reads one dword from `buffers[0]` and splats it across
  `buffers[1]`), which looked like a perfect candidate. But its target buffer `32118` is smaller than
  8 MB, while the surface is 3360×1892×8 ≈ 50 MB. Wrong target.

### A real latent gap found while checking that

`BufferCache::ObtainBuffer` only propagates GPU writes into an aliasing host image for **formatted**
buffers (`bufferCache.cpp:555`):

```cpp
if (is_formatted && is_written) {
    (void)m_texture_cache.InvalidateMemoryFromGPU(vaddr, size, true);
}
```

`descriptors.cpp:115` passes `resource.formatted` straight through, so a **raw** storage-buffer write
that aliases a render target leaves the host image stale. The guard is deliberate — 
`InvalidateMemoryFromGPU` with `formatted_buffer_write = false` deliberately `EXIT`s on
"buffer write aliases GPU-modified image" (`textureCache.cpp:1889-1894`) — so widening it naively would
turn a silent staleness bug into a crash. A probe now reports raw writes that overlap an image, without
changing behaviour, to establish whether this path is actually taken.

### Probe results — the guest programs no fast-clear colour for this surface

Instrumented run, `_gta3_logs/probe_clearwords.stderr.log`. 48 distinct colour render targets reported.
The decisive rows are the three full-res `R16G16B16A16_SFLOAT` targets (`vkfmt=97`, 3360×1892):

```
base=0x2014580000 vkfmt=97 3360x1892 dcc_en=1 dcc=0x2038910000 cw0=0x00000000 cw1=0x00000000
base=0x20452d0000 vkfmt=97 3360x1892 dcc_en=1 dcc=0x2045280000 cw0=0x00000000 cw1=0x00000000
base=0x203e9b0000 vkfmt=97 3360x1892 dcc_en=1 dcc=0x20429b0000 cw0=0x00000000 cw1=0x00000000
```

**`CB_COLOR#_CLEAR_WORD0/1` are zero.** The guest never programs a fast-clear colour for the
separate-translucency surface, so `b38962b` was structurally incapable of producing `(0,0,0,1)` — it
could only ever have cleared to `(0,0,0,0)`. The revert is the correct baseline.

Supporting observations from the same run:

- **The register plumbing works.** `base=0x207de30000` (`vkfmt=44` = `B8G8R8A8_UNORM`, `cmask_fc=1`)
  reports `cw0=0xff000000`, which in `B8G8R8A8_UNORM` *is* `(0,0,0,1)`. Another target
  (`base=0x20719f0000`, 9×1) reports `cw0=0x70e20000`. So clear words are captured correctly when the
  guest actually sets them — the zeros above are real, not a capture failure.
- **CMASK fast clear is rare**: only the two 3840×2160 display targets and one 3360×1892 `B8G8R8A8`
  target set `cmask_fc=1`. DCC is enabled on almost everything else.
- **The DCC-address collision is real**: `base=0x2032eb0000` and `base=0x2035110000` (both `vkfmt=122`,
  3360×1892) share DCC address `0x2034eb0000`. Since `m_surface_metas` is keyed on that address alone,
  any metadata-driven clear would cross-trigger between those two surfaces.
- **Raw writes do alias images, but not this one.** `raw-write-aliases-image` fired only for
  `vaddr=0x20773e0000 size=0x90000` (576 KB) and `vaddr=0x2077260000 size=0x100000` (1 MB), both with
  `gpu_modified=0`. Those are bloom-chain sized; the translucency surface is 3360×1892×8 ≈ 50 MB.

So Kyty sees **no** guest clear of that surface through either the CB fast-clear path or a raw buffer
write.

### Next instrumentation round

The first probe set had a blind spot: only the *raw* branch of `ObtainBuffer` was logged, so a
**formatted** surface-sized write would have been invisible. Three probes now cover the remaining
candidates:

- `[gta3-probe] formatted-write-aliases-image` — formatted GPU writes ≥ 1 MB that alias a host image.
  A ~50 MB hit here means the guest clears the surface through the formatted path, and the question
  becomes whether the cleared bytes reach guest backing before the image re-uploads.
- `[gta3-probe] rejected-nonuniform-image-clear` — fires when `ResolveComputeImageClear` rejects a
  128-bit fill because its four dwords are not identical (`renderCompute.cpp:107-111`). A clear to
  `(0,0,0,1)` in `R16G16B16A16` is `{0x00000000, 0x3C000000, 0x00000000, 0x3C000000}`, which lands
  exactly there. This tests a long-standing suspicion directly instead of assuming it.
- `[gta3-probe] image-clear-candidate` — every single-buffer, write-only dispatch ≥ 1 MB, logged
  *before* the strict shape checks, with format/stride/user-data size. Catches a guest clear shader
  whose shape the matcher does not anticipate.

### Round 2 results — every clear mechanism eliminated

Second instrumented run, `_gta3_logs/probe_round2.stderr.log`:

| Probe | Result |
| --- | --- |
| `rejected-nonuniform-image-clear` | **none** |
| `image-clear-candidate` | **none** |
| `formatted-write-aliases-image` | `vaddr=0x2032eb0000 size=0x18d8000` (~24.8 MB), 32× |
| `raw-write-aliases-image` | only 0x48000–0x240000 ranges (288 KB – 2.25 MB) |

Two hypotheses died here:

- **The uniform-dword rejection theory is wrong.** `ResolveComputeImageClear` never rejected a
  non-uniform 128-bit fill, so the `(0,0,0,1)` → `{0,0x3C000000,0,0x3C000000}` idea — carried for most
  of this investigation — is simply not what happens.
- **There is no compute surface clear at all.** `image-clear-candidate` logs *every* single-buffer
  write-only dispatch ≥ 1 MB before any shape checks, and it never fired.

And inspecting the capture directly kills the last one:

- **There is no clear quad.** Pass 8's draws are all real translucent geometry — event 12481 is 972
  vertices with BC5 normal maps, volumetric fog volumes and a shadow map; event 12553 is 12,000
  vertices. Neither is a 3-vertex fullscreen clear. The depth-test failure seen in `pixel_history` at
  12553 is ordinary geometry occlusion, not a failed clear.

So, exhaustively: no fast-clear colour (`cw0=cw1=0`), no compute image clear, no raw surface-sized
write, no clear quad. **Kyty observes no guest clear of the separate-translucency surface by any
mechanism.**

### The composite model is confirmed exactly

At (1600, 1400) in frame 9321:

```
3419 (separate translucency) = (0.205688, 0.216553, 0.228027, a = 0.0)
3530 (composite output)      = (0.205078, 0.214844, 0.226562)
```

The output equals the translucency RGB to within `R11G11B10` quantisation. `out = A*SceneColor*X + C`
with `A = 0` reduces to `out = C`, so the scene contributes **exactly nothing**. The composite draw
(event 12578) binds `32099` (scene), `649` (1×1) and `3419` — same structure as the night frame, so
this is not a per-frame anomaly.

### Where this now stands

The immediate cause is certain and narrow: **the separate-translucency attachment must start at
`(0,0,0,1)` and Kyty never initialises it.** What is *not* yet known is how the guest expresses that
initialisation, and every cheap instrument is now exhausted.

The next step is a verbose PM4 run — `--printf-direction File` with graphics debug dump enabled — to
inspect the command stream immediately before the separate-translucency pass and look for a
clear-related packet Kyty currently ignores. Kyty already logs a number of "temporary: ignoring …"
cases, so the packet may already be reported and simply unhandled.

Worth following up separately: the `formatted-write-aliases-image` hit. A ~24.8 MB formatted GPU write
over the surface at `0x2032eb0000` (a full-res `R11G11B10`, i.e. scene-colour shaped) causes
`InvalidateMemoryFromGPU` to call `ClearGpuModified` + `MarkBufferModified`, which makes that host image
discard its rendered contents and re-upload from guest backing. Scene colour measured healthy in this
frame, so it is probably landing on the *other* full-res `R11G11B10` surface rather than the live one —
but the mechanism is capable of destroying a rendered target and deserves its own look.

## Part 3 — root cause found: the DCC clear code carries the colour

A PM4 command-buffer dump (`--command-buffer-dump true`, 2200 frames, `_gta3_logs/pm4`) settled it.

### The surface is bound every frame and never cleared by anything visible

In frame 2199's DCB, `CB_COLOR0_BASE` (offset `0x318`) is written with `0x20452c00`, i.e. surface
`0x20452c0000` — the separate-translucency target. The same register burst writes `0x31b` (VIEW),
`0x31c` (INFO = `0x10040730`, DCC bit 28 set), `0x31e` (DCC_CONTROL = `0x00180028`), `0x31f`, `0x321`
— and **never `0x323`/`0x324` (CLEAR_WORD0/1)**, confirming the probe from the guest side.

The same base appears with a stable 4 hits per frame at frames 300, 600, 900, 1200, 1500, 1800, 2199,
so the surface is reused every frame at a fixed address. And nothing writes it: no `0x452c0000` (raw
address, i.e. a `V#` or DMA destination) anywhere in the DCB, the four indirect buffers (`ci`), the
async compute buffer (`a`), or the `cc` buffer. `find_draws` on the target returns exactly four draws
— 972, 426, 612 and 12000 indices — all real geometry, so there is no fullscreen clear quad either.

**Consequence: in Kyty the buffer accumulates across frames.** Each frame loads the previous contents,
then translucency draws blend in — RGB adds up while alpha decays multiplicatively toward zero. That
is why the world looked acceptable at frame ~1167 (night screenshot) and was destroyed by frame ~9117
(daylight): alpha had decayed to 0 and RGB had accumulated to the 0.21 grey measured on the ground.

### The clear is a DCC metadata fill, and the code encodes the colour

The DCC address for that surface is `0x20492d0000`. Searching for its raw form `0x492d0000` finds one
hit, at line 408 of the frame-2199 DCB:

```
IT_SET_SH_REG (OP:0x76) CNT:9
  0x00000240      <- user-data register base
  0x492d0000      <- V# base_lo
  0x00040020      <- base_hi = 0x0020, stride = 4   => 0x20492d0000
  0x0000e000      <- num_records = 57344            => 229,376 bytes (0x38000)
  0x00014204
IT_DISPATCH_DIRECT (OP:0x15) groups = 896 x 1 x 1, mode = 0x41
```

In the RenderDoc capture this is **dispatch 12437**, immediately before the separate-translucency pass
at 12481. Its target buffer `ResourceId::32118` is exactly **229,376 bytes**, matching the `V#`. An
earlier pass over this investigation dismissed that dispatch for writing a buffer "under 8 MB" while
hunting a ~50 MB surface fill — that was the mistake: DCC metadata for this surface is only 224 KB.

Reading the buffer after the dispatch, at both offset 0 and offset `0x37FE0`:

```
00000000: 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40
00037fe0: 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40 40
```

Uniform `0x40` across all 229,376 bytes. On GFX9/GFX10 the DCC clear codes are byte-replicated and
**encode the clear colour directly**:

| DCC code | Meaning |
| --- | --- |
| `0x00` | clear to `(0,0,0,0)` |
| **`0x40`** | **clear to `(0,0,0,1)`** |
| `0x80` | clear to `(1,1,1,0)` |
| `0xC0` | clear to `(1,1,1,1)` |
| `0x20` | clear to the value in `CB_COLOR#_CLEAR_WORD0/1` |
| `0xFF` | uncompressed — *not* a clear |

So the guest asks for exactly `(0,0,0,1)`, and it does so **without** programming the clear-word
registers, because for constant-encoded codes the hardware does not consult them. That is precisely
why `cw0 = cw1 = 0` and why chasing the clear-word registers was a dead end.

### Why each previous state was wrong

- `b38962b` correctly detected the metadata clear and correctly turned it into an attachment clear, but
  then took the colour from `CB_COLOR#_CLEAR_WORD0/1` **unconditionally** — which for a constant-encoded
  clear are meaningless zeros. Result: `(0,0,0,0)`, alpha 0, black world.
- The revert performs no clear at all. Result: cross-frame accumulation, alpha decaying to 0 and RGB
  building up. Better early in a session, worse the longer you play.

### The fix

Decode the DCC clear code and derive the colour from it, per the table above. This also resolves the
safety concern recorded earlier in this document: only recognised *clear* codes become attachment
clears, and `0xFF` (a DCC initialise/decompress) explicitly must not — which was invariant #1.

### Implemented (option 1 — read the fill source)

The fill is a compute dispatch that splats a dword sourced from a companion read-only buffer, and
`TryConsumeComputeMetaClear` skips that dispatch, so the value had to be recovered another way. The
key observation is that the recompiled shader reads `buffers[0][(vsharp[8] >> 2) & 63]`, and that index
is *purely Kyty's own host alignment adjustment* (`descriptors.cpp:114-123`) applied on top of guest
offset 0 — `descriptors.cpp:1020-1025` pushes `user_data` verbatim as the `vsharp` block. So the guest
value simply lives at the read-only descriptor's base address, which makes this robust rather than
shape-fragile.

The change:

- `imageInfo.h` — the DCC code constants plus `DecodeDccConstantClear`, which maps a constant-encoded
  code to a colour and **returns false for anything else**, including `DCC_CLEAR_CODE_REGISTER` and
  `DCC_CODE_UNCOMPRESSED`.
- `renderCompute.cpp` — when a metadata fill is consumed, read the source dword via `TryReadBacking`,
  require it to be byte-replicated, and record it as the clear code.
- `textureCache.{h,cpp}` — `MetaDataInfo` carries `clear_code` / `clear_code_valid`; `ClearMeta` takes
  the code (defaulted, so the HTile callers and tests are unaffected); `MetaClearCode` reads it back.
  `DeleteImage` now erases metadata for any kind, not just HTile, since colour entries exist too.
- `colorRenderTarget.cpp` — register DCC metadata for colour targets, and decode
  `CB_COLOR#_CLEAR_WORD0/1` into `color_clear_value` for the `DCC_CLEAR_CODE_REGISTER` case only.
- `renderDraw.cpp` — at `AcquireRenderTargets`, a recorded code decides the clear: constant-encoded
  codes supply the colour directly, `0x20` falls back to the clear registers, and anything else
  (notably `0xff`) is **not** treated as a clear so the attachment keeps its contents.

This satisfies all three invariants recorded earlier: the written value is inspected rather than
assumed, the colour never comes from unprogrammed registers, and a decompress cannot masquerade as a
clear.

Remaining known risk, unchanged by this work: `m_surface_metas` is still keyed on the metadata address
alone, and two full-res `R11G11B10` surfaces were observed sharing DCC address `0x2034eb0000`. A clear
recorded for one can therefore be consumed by the other. Worth addressing separately by keying on the
range rather than the base address.

For reference, the other two routes considered:

- **Let the dispatch execute** and read the DCC byte from guest backing at bind time. More general, but
  the GPU write must reach guest backing first, costing a download per target per frame plus a
  one-frame lag.
- **CP DMA path.** `BufferCache::FillBuffer` already receives the value directly, so titles that clear
  metadata by DMA would need the same code plumbed through there.

## Part 6 — depth/storage binding ping-pong destroys and rebuilds shadow maps every frame

Chasing the flashing led to a separate, well-evidenced defect.

A probe on `TextureCache::InsertImage` reporting host images recreated at an address they already
occupied showed unbounded growth on shadow-map-sized surfaces — `addr=0x209b310000`, `2048x2048`,
format 124 (`D16_UNORM`), recreated **232 times and climbing** in one short run, alongside the full-res
`R11G11B10` scene colour and a half-res `R16` target.

Tagging the deletion sites attributed **100%** of the repeated destruction to `ResolveDepthOverlap` —
zero from the garbage collector, zero from invalidation. (The GC was the intuitive suspect given its
trigger sits at roughly 2 GiB on a 12 GB card, but it was not responsible.)

Instrumenting the condition gave a single shape, 24 out of 24 identical:

```
binding=1 (Storage)
cached{depth=1 fmt=124 D16_UNORM bpp=2 stencil=0 lay=1 mip=1}
  want{depth=0 fmt=70  R16_UNORM bpp=2 stencil=0 lay=1 mip=1}
```

The branch is `case BindingType::Storage: recreate |= cached.info.IsDepth();`
(`textureCache.cpp:791`). So:

1. the shadow pass renders the surface as **D16 depth**;
2. a compute pass binds the same memory as an **R16_UNORM storage image**, and because the cached image
   is depth the surface is destroyed, recreated as colour, and convert-copied;
3. the next frame's shadow pass binds it as a depth target, the cached image is now *not* depth
   (`recreate |= !cached.info.IsDepth()`), and it is destroyed and rebuilt again.

Every frame, for every shadow map, that is two image creations plus two full-surface conversion copies
at 2048×2048. Vulkan cannot express this directly: a depth format cannot carry `STORAGE` usage, and a
`D16` image cannot be viewed as `R16`, so a single image genuinely cannot serve both bindings.

**Scope of the claim.** This is a strong explanation for the frame rate. It is *not* established that it
causes the flashing: contents do appear to survive, because `CopyImage` routes `D16`↔`R16` through the
dedicated `CopyD16` helper rather than an illegal aspect-mismatched `vkCmdCopyImage`. Flashing remains
unexplained.

**Fix direction.** Stop destroying one representation to build the other. The surface needs a depth
image and a colour alias coexisting, synced on binding change, instead of a single cache slot that
flip-flops. That is a real texture-cache change — the cache currently maps one guest address to one
image — and it should be scoped deliberately rather than bolted on.

## Part 7 — the flashing: the tonemapper samples a stale exposure image

`_RenderDoc/kyty_frame1659.rdc` was captured on a **black frame**, which finally made this measurable.

| Resource | Role | Contents |
| --- | --- | --- |
| `3539` | tonemapper scene input | mean 0.0039, max 0.041 — **populated** |
| `3680` | written by eye adaptation, dispatch 17333 | **1.265625** — a healthy exposure |
| `3731` | **sampled by the tonemapper**, event 17404 | **`(0,0,0,1)`** |
| `3727` | tonemap output | **entirely black** |

Dispatch 17333 reads the 53×30 bloom mip and writes the adapted exposure into `3680`. The value
`1.265625` is exactly what a *working* frame measured earlier (`4451` in `kyty_frame2404.rdc`). So
exposure is being computed correctly — and then the tonemapper samples a **different 1×1 image** that
holds zeros. Multiplying by zero exposure blacks out the whole frame.

`3731` is read **17 times across the frame and never written**. Note also that `3680` returns
`1.265625`, a value above 1.0, so it is a float format, while `3731` is `R8G8B8A8_UNORM`. Two host
images, two formats, one guest resource.

That points at the colour-format split in `ResolveOverlap` (`textureCache.cpp:936-943`): when the
requested format is not `FormatsCompatible` with the cached one it returns a null id, and the caller
creates a **second** image for the same address. Writes to one are then invisible to the other. This is
the same structural fault as Part 6's depth/colour split, but between two colour formats, and it
explains the flashing exactly: when the tonemapper resolves to the freshly written side the frame looks
right, and when it resolves to the stale side the frame goes black.

**Refuted.** Two probes killed this. A probe on the format-incompatibility branch of `ResolveOverlap`
reported **nothing**, and a probe logging every ≤4×4 image creation with its guest address showed that
**every 1×1 image has a unique address** — no two host images share one. `3680` and `3731` are genuinely
different guest resources, so the cache is not splitting anything here.

**Correction to the labelling above.** `3731` is `R8G8B8A8_UNORM`, which is not an exposure format;
Unreal's eye adaptation is float, and the healthy `1.265625` was written to a float 1×1 (`3680`, and the
probe shows several `fmt=109` `R32G32B32A32_SFLOAT` 1×1 images, the usual eye-adaptation set). So `3731`
is far more likely one of Unreal's **1×1 constant textures** — the probe shows many small `fmt=37`
`R8G8B8A8_UNORM` 1×1 images with sizes `0x100`–`0x1000`. Calling it "the exposure image" was wrong.

What survives from Part 7 is narrower but still solid: **the tonemapper multiplies by a 1×1 constant
texture that reads `(0,0,0,1)` when it should read `(1,1,1,1)`**, and that blacks out the frame. The
same 1×1 read `(1,1,1,1)` in working captures.

The open question is therefore not aliasing but **why that texture's contents are missing** — most
likely a guest CPU write that never reaches the host image, which would also explain the intermittency.
The next step is to probe upload/invalidation for tiny CPU-written textures rather than the image cache.

Worth recording: the Part 6 image-recreation probe filtered to `extent.width >= 256`, so it could never
have reported these 1×1 images. The filter was meant to suppress texture churn and instead hid the
resource that mattered.

## Part 8 — the darkness: the real colour-grading LUT is still missing

Two corrections first, both mine.

**The "black frame" was never black.** Part 7 called `kyty_frame1659` a black frame from *looking at a
JPEG*. Measured numerically through the RenderDoc Python API:

```
tonemap_output 3727  3360x1892 B8G8R8A8_UNORM
  r[min=0 max=255 mean=4.724]  g[max=87 mean=4.771]  b[max=103 mean=4.608]
  nonzero_bytes = 299831 of the first 400000
```

Mean **4.7/255** — roughly 1.8% brightness. Very dark, not black. A JPEG of that reads as black.

**The zeroed 1×1 is not a lost texture.** The guest-backing probe shows the tonemapper's `(0,0,0,1)`
1×1 at `0x20047f0000` genuinely contains `0xff000000` in guest memory, next to a white
`0xffffffff` at `0x20047e0000` and a default normal `0xff8080ff` at `0x2010d70000`. These are Unreal's
1×1 dummy textures and Kyty is reproducing them correctly. Part 7's conclusion is withdrawn.

### What the darkness actually is

The colour-grading volume sampled by the tonemapper is **still the neutral fallback** from `667515b`.
Read out of the capture, its grey diagonal is perfectly neutral and matches
`BuildNeutralColorGradingLut` value for value:

```
LUT 3735  32x32x32 R10G10B10A2_UNORM
  red ramp  0.00000 0.00293 0.00782 0.01564 0.02835 0.04888 0.07527 0.10948 ...
  grey diag [0] 0.00000  [4] 0.02835  [8] 0.15054  [12] 0.41838  [16] 0.75953  [20] 0.93842
```

A real game grade is never a perfectly neutral diagonal. And the arithmetic closes exactly:

```
scene 0.0039 x exposure 1.2656 = 0.0049
Unreal log encode -> 0.107
neutral LUT at 0.107      -> ~0.020
                          -> 5.1 / 255      (measured mean 4.724)
```

So nothing downstream is broken. The tonemapper is faithfully applying a neutral curve, and that curve
is far darker in the shadows than the grade the title intends.

**Root cause:** Unreal builds its combined grading LUT with a `WriteToSlice` ES+GS pass that expands a
draw across the 32 slices of the volume. `ShouldSkipGeShader` drops unsupported ES+GS draws, so the
volume is never written, stays all-zero, and `667515b` substitutes the neutral curve. The neutral
fallback was always documented as a stopgap; this quantifies its cost.

This is the same family as the skipped `Clear Surfaces` GS pass in Part 4 — both are unsupported ES+GS
draws — and it lands back on the `graphics-gs-writetoslice-volume` branch found at the very start of
this investigation, which stalled at `1943e1d` on an ES-VS TTMP decode gap. **Finishing that branch is
the fix for the darkness.**

Flashing remains unexplained. It may be easier to judge once the image is correctly exposed, since at
a mean of 4.7/255 almost anything reads as popping in and out.

### Scoping the fix: finishing WriteToSlice

The `graphics-gs-writetoslice-volume` branch already has the right idea. Rather than implementing
general geometry shaders, it renders the WriteToSlice pattern as **layered instanced draws**, emitting
`gl_Layer = gl_InstanceIndex` from the vertex shader — the guest already issues one instance per slice.
By `1943e1d` that worked: the 48³ volumetric fog volumes became GPU-written. Only the 32³ grading LUT
was left, and that is the one this title needs.

Three things stand between here and that fix.

**1. The branch has to be ported by hand.** The change is compact — about 133 lines across 10 files
(`renderDraw.cpp`, `colorRenderTarget.cpp`, the SPIR-V emitter, `shader.{h,cpp}`, `ShaderIR.h`) — but
the branches diverged long ago: 49 commits on the branch and 130 on this one since merge-base
`8fe4765`, and the recompiler moved (`recompiler/ShaderIR.h` is now `recompiler/ir/ShaderIR.h`). A
cherry-pick will conflict throughout, so the changes need re-applying against the current structure.

**2. The TTMP gap is the real blocker.** The LUT's ES vertex shader reads TTMP (trap-temporary) scalars,
which the hardware fills at wave launch. The branch decodes them symbolically but rejects them when
lowering to IR — *"TTMP scalar is launch-supplied state the recompiler does not model"*
(`shaderIR/ShaderIR.cpp:145`). This branch has no TTMP handling at all. Until those values can be
modelled or supplied, that shader cannot be compiled, so the LUT cannot be produced by any amount of
layered-rendering work.

**3. The shader's ISA is not currently obtainable**, which is what the TTMP work needs to be scoped at
all:

- `ShouldSkipGeShader` returns at `renderDraw.cpp:1152`, *before* any shader compilation, so the
  skipped ES/GS pair is never decoded.
- The guest-ISA dumper `DumpShaderRecompilerOriginal` (`shader.cpp:1372`) is dead code: its config gate
  is commented out and it opens with an unconditional `return`. That is why `_Shaders/` holds a single
  stale `.rdna2` from an earlier session.

So the immediate next step is small and well defined: **decode and dump the ES and GS shaders at the
skip point** — the draw already logs `es=` and `gs=` guest addresses — and read what the ES shader does
with TTMP. That determines whether the values can be supplied cheaply (a constant or a system value the
layered path already knows) or whether this needs real launch-state modelling, which would be a much
larger job.

## Part 9 — the WriteToSlice shaders, disassembled: obstacles 2 and 3 above are both gone

Both shaders have now been dumped and disassembled. **Obstacle 2 (TTMP) does not exist, and obstacle 3
is solved.** The remaining work is smaller and much better defined than Part 8 assumed.

### How they were obtained

`ShaderDumpSkippedGeShader` (`shader.cpp`, commit `10bd34f`) writes the raw guest words of both
shaders at the `ShouldSkipGeShader` skip point, into `<shader-log-folder>/skipped_ge/`. It runs at most
twice and deliberately does **not** invoke the recompiler — that draw is skipped every frame, so an
abort inside a decoder would take the game down on every frame.

Disassembly happens offline instead, via a new `shader_disasm` CMake target
(`tests/ShaderDisasmTool.cpp`) that links only the six files in `recompiler/decompiler/` plus `fmt`.
It uses the emulator's own `Decoder::DecodeProgram` / `ProgramToString`, so the output is exactly what
the recompiler would see. Note that MSVC's bundled clang has no AMDGPU target, so no external
disassembler is available on this machine — this tool is the only route.

	cmake --build _Build/v3 --target shader_disasm
	_Build\v3\shader_disasm.exe _Shaders\gta3\skipped_ge\es_*.bin

### There is no TTMP problem

Part 8 recorded the TTMP rejection as "the real blocker". It is not present in these shaders.

- **GS: 275 instructions, 0 unsupported.**
- **ES: 82 decoded, 14 "unsupported" — all past the end of the program.** They are twelve
  `0xbf9f0000` (`s_code_end`, the prefetch padding LLVM emits after `s_endpgm`) followed by two zero
  words. The last real instruction is `s_setpc_b64 s6` at `0x194`.

Neither shader references a trap-temporary register. Whatever prompted the TTMP note on the
`graphics-gs-writetoslice-volume` branch, it does not apply to the shaders this title's LUT actually
uses. **Obstacle 2 is withdrawn.**

### Why the draw is skipped: four conditions, not one

From `skipdraw.log`:

	stages=0x00002030 prim_group=0x0040 vert_group=0x0040 ngg=0x00000001
	max_out=0x000000c0 gs_max_vert=0x00000003 gs_out_prim=0x00000002
	es=0x2008ab0000 gs=0x2008a90000

`stages=0x2030` decodes as `ES_EN=REAL` (bits 4:3=2), `GS_EN=1` (bit 5), `GS_FAST_LAUNCH=1` (bit 13).
The only stage mask `ShouldSkipGeShader` accepts is `0x02002000` — `GS_FAST_LAUNCH` plus
`PRIMGEN_PASSTHRU_EN` (bit 25), i.e. NGG passthrough with no real GS. So four of its conditions fire
independently: `unsupported_stage_mask`, `unsupported_gs_stage`, `gs_max_vert != 0` (3), and
`max_out > 0x40` (0xc0). This is a genuine hardware geometry shader.

`VGT_GS_MAX_VERT_OUT = 3` is `[maxvertexcount(3)]`.

### What the two shaders do — an exact match to UE4 `RasterizeToVolumeTexture`

**ES** = UE4's `WriteToSliceMainVS`. It exports nothing; it writes 7 dwords per vertex to LDS (the
ESGS ring), stride confirmed by `v_mul_u32_u24 v5, 28, v12` — 28 bytes:

| bytes | value | instruction |
|---|---|---|
| 0, 4 | `Position.xy` | `v_mad_f32 v6, s28, v6, s30` / `v_mad_f32 v9, v7, s29, s31` |
| 8, 12 | `UV.xy` | second `buffer_load_format_xy v10, v8, s8, s18`, passed through |
| 16, 20 | `Position.zw` = `(0.0, 1.0)` | `v_mov_b32 v10, 0` / `v_mov_b32 v11, 1.0` |
| 24 | **`LayerIndex`** | **`v_add_nc_u32 v9, s16, v8`** |

`s28..s31` come from `s_buffer_load_dwordx4 s28, s8` and `s16` from `s_buffer_load_dword s16, s8`.
That is line-for-line the UE4 source:

	Output.Vertex.Position = float4(InPosition * UVScaleBias.xy + UVScaleBias.zw, 0, 1);
	Output.LayerIndex      = LayerIndex + MinZ;      // LayerIndex : SV_InstanceID

so `s28..s31` = `UVScaleBias`, `s16` = `MinZ`, and `v8` = the instance ID.

**GS** = `WriteToSliceMainGS`, a pure 3-in/3-out passthrough. Its exports are the whole story:

	exp target=0x0c en=0xf  v0, v1, v2, v3    ; POS0   = position
	exp target=0x0d en=0x4  v0, v0, v7, v0    ; POS1.z = v7  -> render-target array index
	exp target=0x20 en=0xf  v4, v5, v6, v6    ; PARAM0 = UV

`en=0x4` selects only `.z` of the POS1 "misc" export, which on GCN/RDNA is the render-target slice
index (`.x` point size, `.y` edge flag, `.z` layer, `.w` viewport). **`POS1.z` is `gl_Layer`.**

Because the GS neither amplifies nor reorders, ES+GS together are semantically just an instanced
vertex shader that writes `gl_Layer`. This is precisely the shape the
`graphics-gs-writetoslice-volume` branch targeted, and it confirms that approach was correct.

### The chain, closed

`FCombineLUTsPS` builds the 32³ colour-grading LUT through `RasterizeToVolumeTexture`, one instance
per slice. That draw is skipped → the LUT is never written → the neutral fallback from `667515b` is
sampled instead → the frame is dark. The arithmetic already closed in Part 8 (predicted 5.1/255 vs
measured 4.724); the mechanism is now confirmed at instruction level rather than inferred.

### The layered-rendering infrastructure already exists

Checked before scoping the fix, because it was the main cost risk — and it is already present:

- `renderTarget.h:52` — `enum class TargetViewType { Image2D, Image2DArray, Unsupported }`
- `renderTarget.h:66` — picks `Image2DArray` whenever `base_layer != last_layer`
- `colorRenderTarget.cpp:358`, `depthRenderTarget.cpp:342` — `vk::ImageViewType::e2DArray` when
  `layer_count != 1`
- `context.cpp:462` — `rendering.layerCount = state.num_layers`
- `colorRenderTarget.cpp:280,319` — an existing `volume` path for 3D targets
- `renderDraw.cpp:511-631` — `state.num_layers` already computed across attachments

So no new render-target work is expected. The remaining gap is the shader lowering only.

### What is actually left

1. Narrow `ShouldSkipGeShader` so this pattern is no longer rejected: real ES+GS, fast launch,
   `gs_max_vert == 3`, passthrough GS.
2. Lower the pair to one instanced vertex shader. The ES's `ds_write`s at the four known offsets
   become the VS outputs, using the byte→semantic table above; the GS is dropped as a no-op.
3. Route `POS1.z` to SPIR-V `gl_Layer` (`BuiltIn Layer`), which needs the `ShaderViewportIndexLayerEXT`
   capability or Vulkan 1.2 `shaderOutputLayer`.

Steps 1 and 3 are small. Step 2 is the real work — and it is bigger than Part 8 assumed, for the
reason in the next section.

### Correction: the old branch's approach cannot work for this shader

This invalidates part of the Part 8 plan, so it is recorded explicitly rather than quietly dropped.

`graphics-gs-writetoslice-volume` fixes the problem by **compiling the ES as the vertex shader and
adding `gl_Layer = gl_InstanceIndex`** (`c478722`, `1943e1d`). The current tree already compiles the ES
as the VS — `shader.cpp`, `ShaderCompileSpirvVS`:

	const uint64_t shader_addr = regs.es_regs.data_addr;

That is correct for **NGG passthrough** draws (`stages == 0x02002000`), where there is no real GS and
the ES performs its own exports. It does not hold here:

- **This ES exports nothing.** It contains no `exp` instruction and no `s_endpgm`; it ends at `0x194`
  with `s_setpc_b64 s6`, a subroutine return. All it does is write 7 dwords per vertex to LDS.
- **The GS owns every export**, and reads its inputs back out of that same LDS ring — three vertices
  at base `28*v2`, `28*v0`, `28*v3`, each reading offsets 0/4, 8/12, 16/20, 24. That is the exact
  layout the ES writes.

So compiling this ES as a VS and bolting `gl_Layer` on produces a vertex shader with **no position
output**. It would emit valid SPIR-V and draw nothing. The branch's success on the 48³ fog volumes at
`1943e1d` does not carry over — those must take the passthrough path, where the ES does export.

A second, smaller discrepancy: the branch routes `gl_Layer = gl_InstanceIndex`, but this shader
computes `LayerIndex = MinZ + InstanceID`. That is only equivalent when `MinZ == 0`, which is the
common full-volume case but not guaranteed.

The branch's *classification* work (`ClassifyWriteToSliceGs`) and its *SPIR-V layer plumbing* — the
`BuiltInLayer` output, `CapabilityShaderViewportIndexLayerEXT`,
`SPV_EXT_shader_viewport_index_layer`, and the entry-point interface wiring — are both sound and worth
porting as-is. Only the "ES is already the whole vertex shader" assumption is wrong.

### The design that does work: retarget the ES's ring stores into exports

Because the GS is a strict 1:1 passthrough — vertex *i* in, vertex *i* out, no cross-vertex math and no
amplification — the ES+GS pair is semantically a plain vertex shader, and the LDS round-trip can be
elided entirely rather than emulated:

1. **Analyse the GS once** to build a map from ESGS ring byte offset to export slot and component, by
   tracing each `ds_read` into the export operand it feeds. For this shader that map is:

   | offset | export |
   |---|---|
   | 0, 4 | `POS0.x`, `POS0.y` |
   | 8, 12 | `PARAM0.x`, `PARAM0.y` |
   | 16, 20 | `POS0.z`, `POS0.w` |
   | 24 | `POS1.z` → `gl_Layer` |

   Note this is *not* the order a naive struct layout would predict — position is split across offsets
   0–7 and 16–23 with the UV packed between. The map has to be derived, not assumed.

2. **Rewrite the ES's ring `ds_write`s as those exports** and drop the GS. The ES needs no other
   change; its stride is identified by `v_mul_u32_u24 v5, 28, v12`.

3. Reject the pattern (and keep skipping) whenever the GS is not a strict passthrough, so this stays
   safe for any other title that trips the same classifier.

This is a genuine recompiler pass — a GS export-mapping analysis plus an ES store-retargeting
transform — not the ~133-line port Part 8 estimated. The layer plumbing and classifier from the branch
still drop in underneath it.

## Part 10 — WriteToSlice implemented (05 Aug, later)

The design above is implemented. Built, not yet run. Three things came out of doing it that change what
Part 9 recorded.

### 10.1 The hand-read export map in Part 9 is wrong: POS0 and PARAM0 are swapped

The map is now derived by `WriteToSlice::AnalyzePassthroughGs` and it does not agree with the table in
Part 9. The correct map:

| ring offset | export | what the ES puts there |
|---|---|---|
| 0, 4 | **`PARAM0.x`, `PARAM0.y`** | `v_mad_f32` scale-biased pair |
| 8, 12 | **`POS0.x`, `POS0.y`** | raw `buffer_load_format_xy` result |
| 16, 20 | `POS0.z`, `POS0.w` | constants `0.0`, `1.0` |
| 24 | `POS1.z` → `gl_Layer` | `MinZ + InstanceID` |
| — | `PARAM0.z`, `PARAM0.w` | `0` (GS materializes these) |

Cross-checked against UE4's `WriteToSliceMainVS`, which scale-biases the **UV**, not the position:

	Output.Vertex.Position = float4(InPosition, 0, 1);
	Output.Vertex.UV       = InUV * UVScaleBias.xy + UVScaleBias.zw;

so the mad'ed pair at offsets 0/4 is the UV and belongs to `PARAM0`, and the raw pair at 8/12 is the
position. Part 9 read the mad as the position remap and got the two backwards. This is exactly the
failure mode the commit messages warned about, and it is why the map is derived rather than assumed.

### 10.2 The reason the analysis rejected PARAM0 was not control flow

`504708c` recorded the rejection as needing "real dataflow over the CFG". It did not. The decoder fills
`Instruction::dst` from the vdst field unconditionally, and a DS store has no vdst — those bits read as
register 0 — so every `ds_write` looked like it defined `v0` and cleared its provenance. The GS carries
ring[0] in v0 across the bookkeeping stores of the NGG vertex-compaction pass, so PARAM0 lost its
origin there and nowhere else. Stores and exports no longer clobber `dst`.

The linear scan is kept and is sound for this pattern, because conflicting LDS stores poison their slot
instead of overwriting it: a value survives only if every path that wrote it agreed, so a staging copy
that is not uniform across the emitted vertices rejects.

### 10.3 A blocker Part 9 did not see: the ES's entry gate needs NGG launch state

The ES opens with

	s_lshl_b32 vcc_lo, s3, 16
	s_bfe_u64  exec_lo, -1, vcc_lo
	s_cbranch_execz <end>

`s3` is the NGG merged-wave info the hardware supplies when an ES and a GS share a wave; bits 6:0 are
the ES vertex count, and EXEC is derived from them. Kyty's VS user data starts at s8, so s0–s7
initialise to zero — EXEC would come out **empty** and the shader would store nothing at all. Lowered
shaders now get `s3 = wave_size`: every lane of a vertex-shader wave is a real vertex here, and the
wave index in bits 27:24 is zero because the ring slot it addressed no longer exists.

This is the kind of thing that would have looked like "the fix did nothing" from a screenshot.

### 10.4 What the next run should show

Both decisions report to **stderr**, so they survive `--printf-direction Silent`:

- once per ES/GS pair: `[writetoslice] es=… gs=…` followed by the derived ring map and the ES store
  plan, or a reject reason;
- per skipped/admitted draw (first 8): `[writetoslice] classified=… lowerable=… RT=… dim=… depth=…
  slices=[a..b] fan_out=…`.

`classified=0` with `dim=2` would mean the guest binds the volume through a view that does not cover
every slice — the classifier requires the full volume, because the shader's slice index indexes the
view and a partial view would send instances outside it. That is the most likely remaining reason for
the draw to stay skipped, and the `slices=[a..b]` field says so directly.

`lowerable=0` with `classified=1` means the device lacks Vulkan 1.2 `shaderOutputLayer`, or the pair
analysis rejected; the per-pair line carries the reason.

If the LUT is produced, the tonemap stops applying the neutral curve and the shadows should lift. The
flashing is a separate defect (see `gta3-darkness-taa-diagnosis.md`) and is not expected to change.

### Tooling note

The RenderDoc MCP server is not required. `C:\Program Files\RenderDoc` ships a Python module
(`import renderdoc`, version 1.45 here) that drives the same replay API, and the repo already used it
in `_gta3_logs/analyze_renderdoc_targets.py`. Both measurements above were taken that way, so captures
can be analysed whenever the MCP is unavailable.

### The sky rectangle

Still unexplained: a hard-edged blue rectangle in the upper sky, visible in both screenshots.
`Gameface/Content/Common/ProceduralSky/BP_GTA_ProceduralSky` is the natural first suspect.

## Part 5 — root cause of missing characters/vehicles: occlusion queries are discarded

The decisive symptom, reported by the user: **characters and cars only render when the camera overlaps
their model.** That is the signature of broken occlusion queries. Unreal marks primitives whose bounds
contain the camera as unconditionally visible, because they cannot be occlusion-tested reliably;
everything else is culled according to the query result. If every query reports "occluded", only
camera-overlapping objects survive — exactly what is observed.

`graphicsRun.cpp:1551-1557` discards the entire `PIXEL_PIPE_STAT` family:

```cpp
case 0x00000038:   // PIXEL_PIPE_STAT_CONTROL
case 0x00000039:   // PIXEL_PIPE_STAT_DUMP     <- occlusion query result
case 0x0000003a:   // PIXEL_PIPE_STAT_RESET
    LOGF("\t temporary: ignoring unsupported event_write type ...");
    break;
```

On RDNA, `PIXEL_PIPE_STAT_DUMP` is how Z-pass counts are written back — it *is* the occlusion query
mechanism. It fired **127,878 times** in a single run of `_gta3_logs/skipdraw.log`, which is the
per-primitive query traffic of a whole session.

The packet carries a destination address that Kyty throws away. `CpOpEventWrite`
(`pm4Handlers.cpp`) reads only `buffer[0]` and returns `KYTY_PM4_LEN(cmd_id) - 1`, and
`CommandProcessor::TriggerEvent(event_type, event_index)` is not even given the address. From the PM4
dump:

```
0xc0024600  IT_EVENT_WRITE CNT:3
  buffer[0] = 0x00000139     event_type = 0x39, event_index = 1
  buffer[1] = 0x8132a8c0     destination address low
  buffer[2] = 0x00000011     destination address high   => 0x11_8132a8c0
```

Consecutive dumps target addresses 8 bytes apart (`…a8c0` then `…a8c8`), i.e. begin/end pairs of a
64-bit counter, with the guest taking `end - begin` as the visible-pixel count. Because nothing is ever
written, the guest reads whatever is already in that memory — giving a delta of zero, i.e. "occluded",
for every primitive.

This also fits the secondary symptoms: intermittent appearance and flashing follow from stale
uninitialised memory changing under the query addresses, and frame 1605 simply captured a moment when
the character was culled.

### Implemented

Both fixes landed, in sequence:

- `f98bc33` — parse the destination and write a monotonically increasing counter, so every
  `end - begin` is positive and every primitive reports visible. Characters and vehicles appeared
  immediately, but with occlusion culling effectively disabled the frame rate fell from ~28 to 8 fps.
  Note the ~28 fps baseline was *not* healthy: it was fast because nearly everything was being culled,
  including the player and all traffic.
- `06927c6` — real `VK_QUERY_TYPE_OCCLUSION` queries, with the counter retained as a fallback.
- `760db0f` — corrects `06927c6`. It began the query at the dump and refused the sample when no render
  pass was open, but Kyty opens the render pass **lazily in the draw path**
  (`renderDraw.cpp` `BeginRendering`), so no instance exists when the begin dump arrives and *every*
  sample was refused. The always-visible fallback ran unchanged. Split across the three points where
  the information exists: the begin dump arms the query, the draw path brackets it with
  `beginQuery`/`endQuery`, and the end dump binds the completed slot.

### Measured effectiveness

Instrumented run, final sample:

```
serviced=49152  fallback=13306  occluded=6805  visible=11006
```

- **38% of resolved queries report occluded** (6805 / 17811), so culling genuinely works.
- **21% of samples fall back** (13306 / 62458) to always-visible. Those are conservative — they add
  some over-draw but never cull wrongly. The fallback fires when no draw was bracketed between the two
  dumps, so reducing it means understanding why some armed queries see no draw.
- `serviced` counts both begin and end dumps, so completed queries are roughly half that figure.
- No Vulkan validation output, i.e. no query-scoping violations.

The conclusion for performance work: over-draw from broken occlusion is **no longer** the dominant
cost. Further effort there has limited headroom; the remaining gap is general emulator cost.

The effectiveness probe in `context.cpp` is temporary and should be removed once this is settled.

### The two options, as originally scoped

1. **Conservative (small).** Parse the destination from `buffer[1]`/`buffer[2]` and write a
   **monotonically increasing 64-bit counter** on each `PIXEL_PIPE_STAT_DUMP`. Every `end - begin`
   delta is then positive, so every primitive reports visible. Interleaved queries stay correct because
   a monotonic counter yields a positive delta regardless of ordering. This disables occlusion culling
   — a GPU cost, since Unreal then draws everything — but it can never wrongly cull.
2. **Correct (larger).** Real Vulkan occlusion queries: a `VK_QUERY_TYPE_OCCLUSION` pool,
   `vkCmdBeginQuery`/`vkCmdEndQuery` bracketing the guest's query range, and asynchronous readback
   into the guest address. Preserves culling but needs query-pool lifetime management and a result
   path that does not stall the GPU thread.

Option 1 is the right first move: it is a few lines, it directly removes the false culling, and it
gives a correct baseline that option 2 can later optimise.

## Part 4 — missing character and vehicles (superseded by Part 5)

> The analysis below predates the occlusion-query finding and is kept for the evidence it records. Its
> conclusion — that skeletal draws are simply never submitted — is explained by Part 5: the draws are
> culled by failed occlusion queries, not absent from the engine.

With the DCC clear fix in place the world renders, but the player character and street vehicles are
absent. Evidence from `_RenderDoc/kyty_frame1605.rdc` (captured with the player standing next to a car)
and `_gta3_logs/skipdraw.log` (a `--printf-direction File` run):

1. **They are not rasterised.** The G-buffer normal target `3146` contains road, kerb, bridge, distant
   city and road markings, and no character or vehicle anywhere in frame. The albedo target `3476` is
   entirely black, which is worth a separate look.
2. **Kyty is not dropping them.** In the whole 620 MB log there are only two skip categories: 32×
   `Skipping unsupported GE shader draw` (all *one* shader pair, under a `Clear Surfaces` marker) and
   32× `skipping zero-sized dispatch` (four shaders, `groups=0x1x1`, which the guest itself asked for).
   There are **zero** `null storage` or `unmapped` hits, so the storage-buffer clamp from `5b080c0` is
   not collapsing skinned vertices — that hypothesis is dead.
3. **No skinned draw exists in the frame at all.** Every base-pass draw uses the same UE4 GPU-Scene
   static-mesh layout — `in_attr_0` = `R32G32B32_FLOAT` position at stride 12, plus a per-instance
   `R32_UINT` id (checked on event 3727). There are no bone-weight or blend-index streams anywhere.

**Correction — do not read the above as "skeletal meshes never render".** A later screenshot at frame
2432 shows the player character rendering correctly and in full: body, arms, legs, head all present and
correctly positioned. The three observations above are accurate *for frame 1605 specifically*, but the
conclusion drawn from them was too strong. Whatever is happening is **intermittent and
camera-dependent** — the user reports it appearing after moving the camera for a while — not a
permanent absence of skeletal-mesh submission.

That reframes the problem: it is not "skeletal draws are never submitted" but "skeletal draws are
sometimes absent". Frame 1605 captured one of the bad moments. A capture taken *while the character is
visibly missing* is what would actually pin this, and frame 1605 may already be that — but it must be
compared against a good frame rather than treated as the steady state.

Caveat on the log evidence: the skip counter caps at 32 emissions and all 32 shared one signature, so a
*later* different signature would be invisible. Raising that cap is cheap if this needs ruling out.

### Two smaller findings from the same log

- **A GS-based `Clear Surfaces` pass is skipped** — `stages=0x00002030`, `gs_max_vert=3`,
  `max_out=0xc0`, `es=0x2008ab0000`, `gs=0x2008a90000`. It is a *clear*, and this investigation has
  shown how much damage a missing clear does, so it is a plausible contributor to remaining lighting
  oddities. Same class of unsupported ES+GS draw that the stalled
  `graphics-gs-writetoslice-volume` branch was built for.
- **Depth bias is not implemented** — 8× `ignoring polygon offset context register (depth bias not
  implemented)`. That produces shadow acne and decal z-fighting, which reads as "lighting looks off".

### Dithered opacity is not resolving (frame 2432 screenshot)

In the frame where the character does render, its shorts and legs show a strong regular stipple /
checkerboard pattern, and the earlier daylight frame showed heavy blocky dithering across the whole
image. Unreal uses **dithered opacity** for LOD fade and masked materials and relies on **TAA** to
resolve it into smooth coverage. A persistent stipple is the classic signature of TAA not accumulating.

The user additionally reports the character **flashing** — present some frames, completely hidden on
others. A symptom that alternates per frame points at a double-buffered resource, and the TAA history
is exactly that.

Traced in `kyty_frame1605.rdc`:

| Event | Role | Resource |
| --- | --- | --- |
| 5349, 5393 | separate-translucency composite | writes `3535` |
| 5366 | **TAA compute** | reads scene `3535`, reads **history `3587`**, writes **`3538`** |
| 5796 | tonemap | reads **`3538`** |

`3587` is **only ever read** in this frame — four usages (5230, 5252, 5324 in the separate-translucency
pass, and 5366) and never written. `3538` is written by TAA and consumed by the tonemapper. So
`{3587, 3538}` is a **ping-pong pair whose roles swap every frame**: next frame TAA should read `3538`
as history and write `3587`, and the tonemapper should follow.

That is the leading hypothesis for both symptoms: if Kyty mishandles one side of that pair — a fresh
host image created for one of them, the two aliasing one guest allocation but backed by separate host
images, or the swap not being tracked — then every other frame TAA blends against a wrong or empty
history. Alternating frames is precisely "flashing", and a history that is wrong half the time also
prevents dithered opacity from ever resolving. Note this is the same *class* of bug as the DCC clear:
a resource whose per-frame state Kyty does not reproduce.

The history sampled non-empty in this frame (mean ≈ 0.0066), so it is not simply zero — which is why
this needs two consecutive frames to confirm, not one.

A stray teal curved line across the same frame is unexplained and may be unrelated.

**Next step.** Two consecutive captures, or one taken on a bad frame, to see which side of the pair is
wrong — currently blocked by the capture crash. A capture-free alternative is to log, per frame, the
host image ids Kyty resolves for the TAA history and output plus whether either was freshly created;
if one side is recreated on alternate frames its contents are undefined, which would confirm this
outright.

### Capturing now crashes

RenderDoc capture reportedly crashes the emulator as of this build, which blocks the obvious next step.
That needs fixing (or working around) before the intermittent-character and dithering issues can be
investigated properly.

### A caution recorded for future work

Post-VS output is not a reliable visibility test on its own here. Draw 3709 looked clipped (`w` negative
on its first vertices), but draw 3116 — which demonstrably renders — has the same constant `z = 10.0`
and off-screen leading vertices. Sampling the first few vertices of a mesh says nothing about whether
the draw covers pixels.

### Why the config route was abandoned

The title's exposure settings are **not shipped as `.ini` anywhere**:

- The pak contains exactly **10** `.ini` files in total (verified by extracting with `-Filter="*.ini"`):
  eight under `Engine/Config` or `Engine/Platforms`, plus `Gameface/Config/DefaultInput.ini` (78 bytes)
  and `OriginalData/GTA3/gta3.ini` (10 bytes). There is no `DefaultEngine.ini`,
  no `DefaultScalability.ini`, and no cooked `Gameface/Config/PS5/PS5Engine.ini`.
- Nothing on disk outside the pak either — only `crashreportclient.ini` and a manifest.
- The config is not baked into `eboot.bin` as ini text: Ghidra finds no `[/Script/...]` section header
  anywhere, and `RendererSettings` appears only as the `URendererSettings` class name string.

So the intended exposure values are not recoverable from shipped data by any of the three routes
tried. Measuring a daylight frame is the only cheap way to check exposure behaviour.

### UnrealPak filter caveat

`UnrealPak <pak> -Extract <dir> -Filter="a,b,c"` **only honoured the first comma-separated pattern** in
this build. A run with `-Filter="*.ini,*GTA3World.umap,*ProceduralSky*,*PostProcess*,*TimeOfDay*,*Weather*"`
extracted the 10 `.ini` files and nothing else, even though `BP_GTA_ProceduralSky.uexp` is known to be
present. Run one pattern per invocation. Also note an earlier unfiltered-ish extraction produced 4,992
`.uexp` against only 5 `.uasset`, so asset headers need to be requested explicitly — and a `.uexp`
without its `.uasset` header cannot be parsed.

## What the guest binary can and cannot tell us (Ghidra)

`eboot.bin` loaded and analysed in Ghidra: ELF, x86-64, image base 0, 88,292 functions, 236,993
symbols, 121 MB of memory. `ue4commandline.txt` is
`../../../Gameface/Gameface.uproject GTA3World`, so the UE4 project is `Gameface`.

Useful confirmations:

- Unreal's separate-translucency path is present — strings `SeparateTranslucency`,
  `SeparateTranslucencyColor`, `SeparateTranslucencyModulateColor`, and the GBuffer visualisation
  list containing `SeparateTranslucencyRGB` / `SeparateTranslucencyA`.
- The full `PostProcessSettings.AutoExposure*` and `r.DefaultFeature.AutoExposure.*` cvar set exists,
  including `ExtendDefaultLuminanceRange`.

Limitations found the hard way:

- **The build is stripped of UE4 symbols.** `search_functions("Translucen")` returns nothing; the
  symbols are auto-generated `FUN_*` labels plus imports/exports. Locating specific render code means
  string-xref archaeology across 88k functions.
- The three xrefs to the `"SeparateTranslucency"` string land in `FUN_038bed50`, which decompiles to
  227 KB and turns out to be Unreal's **show-flag dumper** (`"%s=%d"` over flag names), not the render
  target allocation. It does not carry the clear colour.
- **The engine config is not on disk.** Only `crashreportclient.ini` and a manifest are loose;
  everything else lives in `gameface/content/paks/pakchunk0-ps5.pak` (4.86 GB, single chunk). Reading
  the title's auto-exposure settings would need a UE4 pak extractor, and a shipping PS5 pak is likely
  AES-encrypted.

### Extracted pak contents

A partial extraction of `pakchunk0-ps5.pak` exists at
`gameface/content/paks/extracted` (9,840 files: 4,992 `.uexp`, 4,313 `.ubulk`, 30 `.umap`, 10 `.ini`).
It does **not** settle the exposure question:

- The only file in the whole extraction mentioning `AutoExposure` / `r.DefaultFeature` is
  `Engine/Config/BaseEngine.ini`, and there it appears solely as Matinee property redirects — no
  values.
- The title's own `DefaultEngine.ini` is absent; `Gameface/Config` contains only `DefaultInput.ini`.
- The 30 extracted `.umap`s are geometry sublevels (`comN*`, `indust*`, `land*`). Spot-checking
  `industNE.umap` finds `Volume` and `StaticMesh` in the name table but no `PostProcess`, `Exposure`
  or `Fog`, so the persistent map and its post-process volumes are not in the extracted set.

Two assets are worth remembering: `BP_GTA_ProceduralSky.uexp` (references `SkyLight`) and `BET.uexp`
(references `PostProcess`). `BP_GTA_ProceduralSky` is the natural first suspect for the hard-edged
sky rectangle, though reading blueprint logic would need a real UE4 asset parser.

Conclusion on tooling: for a *rendering* defect, RenderDoc plus emulator-side instrumentation is
strictly more informative than the guest binary, because it shows what the GPU was actually told to
do — which is the ground truth Kyty has to reproduce. Every load-bearing fact in this document came
from that route. Ghidra is the right instrument for guest **CPU** questions instead, for example the
still-open read from `0xfffffffffffffff8` at guest instruction `0x0000000900643fa0` noted in
`gta3-runtime-fixes.md`; with Kyty's module base at `0x900000000` that is file offset `0x643fa0`,
which is directly decompilable.

## Notes for whoever picks this up

- The world being dim (`92766` mean ≈ 0.004) is expected: the in-game clock reads 05:13 and Unreal's
  auto-exposure (buffer 4451 = 1.2656) compensates downstream. That is not the defect.
- `3585` and `92766` are both full-res `R11G11B10` host images, and `3588`/`3637`/`3475`/`4434` are
  all full-res `R16G16B16A16`. They are distinct surfaces in a legitimate Unreal chain, not
  aliasing bugs — reads and writes line up once the pass structure above is understood.
- The unfinished `graphics-gs-writetoslice-volume` branch (up to `90639dc`) is a separate effort to
  execute the Unreal `CombineLUT` geometry-shader pass so the *real* grading LUT is produced. It
  stalled on an ES-VS TTMP decode gap (`e1a3284`, `768fecb`). It is unrelated to this black-world
  defect; the neutral-LUT fallback from `667515b`/`d79fe78` is already working.
