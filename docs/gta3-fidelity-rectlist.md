# GTA III: The Definitive Edition — Fidelity-mode rendering gaps

**Emulator:** KytyPS5 (Vulkan backend, GCN→SPIR-V shader recompiler)
**Branch:** `fix/gta3-level-load_v3`
**Game path:** `Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin`
**Trigger:** in-game **Graphics → Fidelity** (Performance mode does not hit either issue)

This document is **evidence-based**, matching the convention of
[`gta3-definitive-issues.md`](gta3-definitive-issues.md): each item states the **observed symptom**
(verbatim log lines / measurements), the **confirmed root cause**, the **fix** (if made), and
labels anything not directly observed as `UNVERIFIED`.

---

## 0. Why this only appears in Fidelity mode (fact)

Setting Graphics → *Fidelity* (with Bloom / Lens Flare / Motion Blur / Depth of Field **On**)
makes the game allocate and draw a post-processing chain that *Performance* mode never creates.
Two distinct gaps are hit, in this order. The first is fixed; the second is not.

Timeline evidence that these are settings-driven and **not** a regression from the surrounding
work on this branch: ~100 headless runs before the settings change never produced either abort and
reached frame ~4440 at a sustained 60 fps; every run after the switch reproduces them. The settings
live in `_SaveData\ue4savegame.dpx.sav`, so they persist across launches — including headless ones.

Fidelity is also far more expensive here: ~7.6 fps versus ~60 fps in Performance on an RTX 4070.

---

## 1. `unknown format: format = 34 … tile = 27` — FIXED (`350f83c`)

### Symptom (fact)

```
--- Error ---
unknown format:
format = 34
width  = 1680
height = 946
pitch  = 1680
levels = 1
tile   = 27
 in src\graphics\guest_gpu\tile.cpp:1597
```

### Root cause (confirmed)

`format = 34` is `Prospero::BufferFormat::k11_11_10UInt` (`gpu_defs.h:334`; the enum starts at
line 299, so this is the `BufferFormat` space, which is what the `TileGetTextureSize` callers pass
as `guest_format`).

That value had **no entry** in `kFormatInfo` (`gpu_format.cpp`) — only `k11_11_10Float` (36) was
listed. So:

1. `Prospero::NumBytesPerElement(34)` returned `0`;
2. `GetTextureBlockLayout(34, 27, …)` (`tile.cpp:419`, the `tile == 24 || tile == 27` branch) bails
   on `out.bytes == 0` and returns false;
3. `TileGetTextureSize` falls through every supported path and hits the `EXIT` at `tile.cpp:1597`.

`1680x946` is exactly half of the `3360x1892` scene, i.e. the half-resolution chain the
post-processing passes run on — which is why only Fidelity allocates it.

### Fix

Added the missing table entry and Vulkan mapping. The packing is a 32-bit 11:11:10 word, identical
to `k11_11_10Float`, so **every size and tiling calculation is the same** — that part is
unambiguous.

`UNVERIFIED (interpretation)`: Vulkan has **no** integer 11:11:10 image format, so the view is
reinterpreted as `VK_FORMAT_B10G11R11_UFLOAT_PACK32`. Storage is bit-identical, but a true integer
fetch would return different numbers. The entry is therefore flagged as a float format rather than
an integer one. If a shader turns out to do manual pack/unpack against this surface it will need a
different mapping (`VK_FORMAT_R32_UINT` preserves the bits with integer semantics but collapses the
three channels to one). **This wants visual validation against a Fidelity frame.**

---

## 2. `unsupported host-expanded rect-list draw` — OPEN

### Symptom (fact)

With §1 fixed, Fidelity reaches the next gap. 4/4 runs, deterministic, during the intro:

```
--- Fatal Error ---
Not implemented (!IsHostExpandedRectListDrawSupported(vs_input_info, draw, emit))
 in src\graphics\host_gpu\renderer\renderDraw.cpp:1042
```

The abort now reports its inputs (`b5b3ea1`):

```
unsupported host-expanded rect-list draw: index_count=63 draw_vertex_count=4 vs_buffers=0 instance_count=1
```

### Root cause (confirmed)

`renderDraw.cpp:1002`:

```cpp
static bool IsHostExpandedRectListDrawSupported(const ShaderVertexInputInfo& vs_input_info,
                                                const DrawCallInfo& draw, const DrawEmitInfo& emit) {
    if (!emit.draw_prim7_as_ngg)      { return true; }   // non-NGG path handles it
    if (vs_input_info.buffers_num != 0) { return false; } // needs a procedural VS
    return draw.index_count == 3 || draw.index_count == emit.draw_vertex_count;
}
```

`draw_prim7_as_ngg` is `Config::NggRectlistDrawEnabled() && primType == kRectList`, and
`ngg_rectlist_draw_enabled` **defaults to `true`** (`emulatorConfig.h:38`). For that path
`GetDrawTopology` selects `eTriangleStrip` and `draw_vertex_count` is hard-coded to `4`
(`renderDraw.cpp:1390`) — i.e. the NGG expansion handles **exactly one rectangle**: 3 guest
vertices expanded to a 4-vertex strip.

GTA III's Fidelity post-processing issues **one auto-draw containing 21 rectangles**:
`index_count = 63 = 21 × 3`. `63` is neither `3` nor `4`, so the guard rejects it.

`vs_buffers = 0` confirms the vertex shader is **procedural** — it derives positions from
`gl_VertexIndex` rather than fetching attributes. That is precisely why the guard demands
`buffers_num == 0` for the NGG path in the first place.

### Workaround (verified)

```
kyty_emulator.exe --ngg-rectlist-draw false …
```

Verified: Fidelity runs with no error (reached frame 1177). This takes the non-NGG path, which
selects `eTriangleList` and draws the raw `index_count` vertices.

**Caveat:** 63 vertices as a triangle list is 21 *triangles*, not 21 *rectangles* — each rect is
drawn as the single triangle formed by its three given corners, so it covers roughly half its
intended area. For a fullscreen effect whose three corners already over-cover the screen this is
invisible; for tiled bloom/DoF rects it may show. `UNVERIFIED`: not yet checked visually.

### How to fix properly

The limitation is only the single-rect assumption, so the shape of the fix is small; the risk is in
the vertex indices, not the plumbing.

**The easy part (~10 lines).** Relax the guard to accept `index_count % 3 == 0` and emit one
4-vertex triangle strip per rect instead of one strip total, in `EmitDrawPrimitives`
(`renderDraw.cpp:1036`).

**The catch — why a naive loop is wrong.** Rect-list semantics derive the fourth corner as
`v3 = v0 + v2 - v1`. The current single-rect case works because the procedural VS is asked for
`gl_VertexIndex` values `0..3`, where `3` is one past the end of a 3-vertex draw and the shader
produces the derived corner. For rect *r* the same trick would ask for index `3r + 3`, which
**collides with the first corner of rect r+1** — so the loop would silently render wrong geometry
rather than fail. A correct implementation needs one of:

1. a **generated index buffer** mapping host quad corners to guest vertices
   `{3r, 3r+1, 3r+2, derived}`, plus a way for the shader to recognise the derived corner; or
2. **per-rect draws** where the derived corner is identified by something other than "index one
   past the end" — e.g. a push-constant/spec-constant carrying the rect base so the VS can compute
   `corner = gl_VertexIndex - base`.

Option 2 is the smaller change and keeps the existing procedural-VS contract; option 1 is a single
draw call and therefore better for the 21-rect case, but requires shader specialisation.

Either way this is **moderate, not trivial** — a focused change in the draw/shader path, and it
must be validated visually against a Fidelity frame, because the failure mode of getting the index
mapping wrong is *wrong pixels*, not a crash.

### Reproducing

```
build.bat dev
.\drive_gta3.ps1 -IntroSeconds 30 -PostStartSeconds 40 -Build windows-nolauncher
```

Requires Fidelity to be selected in-game first (it persists in `_SaveData`). Add
`-ExtraArgs @('--ngg-rectlist-draw','false')` to exercise the workaround. The
`unsupported host-expanded rect-list draw` line in `_gta3_logs\gta3_<tag>.out.log` reports the
draw parameters.

---

## 3. Not caused by Fidelity (context)

Two problems reproduce in **both** modes and are tracked separately; do not confuse them with the
above when triaging a Fidelity log:

- **The 3D world does not reach the screen.** A frame capture
  (`_RenderDoc\kyty_frame2120.rdc`) shows the tonemap pass sampling HDR image `7350`, which is
  never rendered into during the frame, while the scene is rendered into image `7532` — both
  `3360x1892 R16G16B16A16_SFLOAT`. Same guest surface, two different cached images: writer and
  reader disagree. This is the larger issue and is independent of the graphics preset.
- `BufferCache: recursive cache lock acquisition` and an intermittent access violation at
  `runtimeLinker.cpp:1032`, both predating this work.
