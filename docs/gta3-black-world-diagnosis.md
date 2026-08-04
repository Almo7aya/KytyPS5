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

### The sky rectangle

Still unexplained: a hard-edged blue rectangle in the upper sky, visible in both screenshots.
`Gameface/Content/Common/ProceduralSky/BP_GTA_ProceduralSky` is the natural first suspect.

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
