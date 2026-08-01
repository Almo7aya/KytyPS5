# GTA III: The Definitive Edition — the world renders black

**Emulator:** KytyPS5 (Vulkan backend, GCN→SPIR-V shader recompiler)
**Branch:** `fix/gta3-level-load_v3`
**Game path:** `Z:\projects\PS5\games\Grand.Theft.Auto.III.The.Definitive.Edition\eboot.bin`
**Mode:** Graphics → **Performance**, all options Off (Fidelity has its own separate gaps — see
[`gta3-fidelity-rectlist.md`](gta3-fidelity-rectlist.md))
**Status:** OPEN

This document follows the convention of [`gta3-definitive-issues.md`](gta3-definitive-issues.md):
each claim is labelled **(fact)** when it comes from a measurement or a direct observation, and
`UNVERIFIED` when it is inference. Its purpose is to record the investigation state accurately —
including where our own reasoning is weaker than it has been treated — so the next attempt does not
re-run experiments that have already been done, or trust conclusions that were never actually
established.

**This is not a regression.** The user confirmed the behaviour predates the `upstream/main` merge
on this branch.

---

## 1. The symptom (fact)

The game launches, reaches level load, and holds ~60 fps in Performance mode on an RTX 4070. The
3D world does not appear on screen; the frame is black.

## 2. The pipeline

GTA III Definitive is a deferred renderer. Shape of a frame, from RenderDoc captures:

```
geometry  → G-buffer, 6 attachments @ 3360×1892
          → lighting → HDR scene buffer (R16G16B16A16 / R11G11B10)
          → bloom mip chain
          → tonemap → display buffer @ 3840×2160 → swapchain
```

---

## 3. What has been measured

### 3.1 The G-buffer is correct (fact)

RenderDoc screenshots of FB1/FB2/FB3, inspected by the user, show textured road surface, lane
markings, barriers and buildings. Geometry, vertex shaders, texture decode, tiling and
rasterization are all working. **This is the strongest fact in the investigation** — it is a direct
visual observation, not an inference from instrumentation.

### 3.2 The tonemap samples a surface that RenderDoc reports as read-only (fact)

The tonemap pass samples a full-resolution `R16G16B16A16` surface. RenderDoc
`get_resource_usage` on that image reports reads only, across the whole captured frame — no colour
target, no storage image, no compute write.

### 3.3 The guest does program that address as a colour target (fact)

`HW::Context::SetColorBase` was instrumented. The address the tonemap samples **is** written into a
`CB_COLOR` base register by the guest (`programmed_as_CB_base=YES`). The guest intends to render
into it.

### 3.4 No draw ever resolves it (fact)

No `ResolveRenderColorTarget` call ever produced an attachment at that address, across an
instrumented session.

### 3.5 The cache image identity is stable (fact, but see §5.1)

The reader's image identity does not churn between frames (observed stable at e.g. `42.1`).

---

## 4. Ruled out by measurement

Each of these was tested and produced a negative result. Do not re-run them without a reason.

| Hypothesis | How it was ruled out |
|---|---|
| Texture-cache split / identity churn | Reader identity stable across frames (§3.5) — **but see §5.1** |
| Lost history buffer | Instrumented; not the cause |
| Rect-list draws being skipped | Counter read **0** with the flag set. `--ngg-rectlist-draw false` was also tested directly by the user: *"no it does not render, it's kinda the same as the old results"* |
| Draws dropped by the framebuffer check | Reduced 128 → 3 by the `CB_TARGET_MASK` fix; the world stayed black |
| Colour-write masking | Fixed (`color_write_enable` now consistent with the resolved target mask); no change to the symptom |
| Null-image substitution hiding a real target | All substitutions were genuine `addr=0` |
| PM4 predication | Disabled **entirely**, not merely bypassed; no change |
| CB resolve mode 3 | Checked; not involved |
| MRT attachment compaction | See §4.1 — instrumented this session, negative |

### 4.1 MRT attachment compaction (fact, negative)

`renderDraw.cpp:847` packs resolved colour targets contiguously into `state.color_info[]`, while
the fragment shader's SPIR-V output locations are the **guest MRT slot numbers** (the recompiler
emits `out_mrt_<index>` from `inst.export_info.index`, see
`ShaderInfoCollection.cpp:189`). A gap in the resolved slots would therefore misroute every export.

Vulkan validation reported `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering`, which looked
like exactly this. A probe was added to fire whenever the resolved slot mask was non-contiguous.

**It never fired.** Resolved slots are always contiguous from 0, so the packed order and the slot
order agree. The validation warning is the benign case: the shader exports MRT1 while the guest
binds only `CB_COLOR0`, and real hardware discards that write too.

A comment recording this measurement is in `renderDraw.cpp` so the code does not look like a
latent bug to the next reader.

---

## 5. Weak joints in the reasoning

This section exists because two conclusions in this investigation have already had to be walked
back. These are the places where the remaining reasoning is thinner than it has been treated.

### 5.1 "No writes" is VkImage-scoped, not address-scoped

§3.2 establishes that *this VkImage* was never written. If the writing pass obtained a **different
VkImage for the same guest memory** — through a cache split, a differing extent/pitch
interpretation, or a depth alias — RenderDoc would report precisely what we see, and there would be
no contradiction at all: just two objects where there should be one.

§3.5 only shows the *reader* is self-consistent. It says nothing about the writer. An earlier
"not a split" conclusion was already withdrawn for being too narrow (it examined a single
address/extent pairing; splits did exist at others).

**Treat the split hypothesis as open.**

### 5.2 The write address and the read address are computed by different code paths

`ResolveRenderColorTarget` takes a `render_target_slice_offset` parameter. The attachment address
is the programmed `CB_COLOR` base *plus* whatever that offset resolves to. The reader derives its
address from a T# texture descriptor through an entirely separate path
(`RenderExecutor::ResolveTexture` in `descriptors.cpp`).

If those two arithmetic paths disagree — slice offset, tiling swizzle, a base shift — then §3.3
("programmed as a CB base") and §3.4 ("nothing ever writes that address") are **both true
simultaneously, with no missing draw anywhere**. Nothing run so far would have detected this.

`UNVERIFIED` — but nothing rules it out, and it is cheap to test (§7.2).

### 5.3 We never confirmed what the surface is

It has been called "the scene colour buffer" throughout, on the grounds that the tonemap samples
it. It could equally be a **TAA history buffer** or an **exposure/adaptation target**. That changes
the diagnosis completely: a history buffer being empty is expected on the first frame, and the real
bug would be that it never accumulates.

---

## 6. Hypotheses still standing

### 6.1 Address mismatch between writer and reader

See §5.2. Currently the hypothesis with the best ratio of explanatory power to cost of testing.

### 6.2 Cache split — writer and reader hold different VkImages

See §5.1. Would fully explain §3.2 + §3.3 + §3.4 with no missing draw.

### 6.3 The write is not a draw

A clear, a DMA/copy, or a compute dispatch that writes the surface without going through
`ResolveRenderColorTarget` would be invisible to the §3.4 instrumentation, which only counts draw
resolves. `UNVERIFIED`.

### 6.4 Exposure, not a missing write

Reframing worth taking seriously. Auto-exposure reduces scene luminance into a small buffer and the
tonemap multiplies by it. **If that value is zero, the tonemapped output is black while the HUD —
composited after tonemap — survives untouched.** That matches the symptom shape.

Supporting circumstantial evidence: Vulkan validation reports
`Undefined-Value-StorageImage-FormatMismatch-ImageView` on a **`vkCmdDispatch`**, with
`storage_uint_2d_array` declared `R32ui` against an `R8_UINT` image view — *"any loads or stores
with the variable will produce undefined values to the whole image (not just the texel being
accessed)."* An R32ui storage image written by a compute pass is the shape of a luminance
histogram.

`UNVERIFIED` and important to state honestly: that warning was **attributed** to the 1×1 null-dummy
image path (`TextureCache::GetNullImage`, `textureCache.cpp:518`, which keys the dummy on pixel
format alone). That attribution was never proven — the probe that ran checked only the
*storage-usage* condition (`VUID-VkWriteDescriptorSet-descriptorType-00339`), not the format
mismatch, and it did not fire.

---

## 7. Proposed next experiments, cheapest first

### 7.1 Clear new colour images to magenta

One-line change, no RenderDoc needed. Anything magenta on screen was **never written**; anything
black **was written with black**. These are two completely different bugs that currently look
identical, and this separates them in a single run.

### 7.2 Address-keyed write/read ledger

Per frame, record every `(address, size)` attached as a colour target and every `(address, size)`
sampled by the post-processing chain, then diff the two sets.

This is the highest-value experiment because it **sidesteps VkImage identity entirely**. If one
pass writes address X and another reads address X but they landed on different VkImages, an
address-keyed ledger shows it and RenderDoc structurally cannot (§5.1). It also detects the
slice-offset mismatch of §5.2 for free.

Run 7.1 and 7.2 together in a single session.

### 7.3 Live `CB_COLOR` base at each draw

Log the live `CB_COLOR` base at every draw alongside the programmed one. Separates "the base is
overwritten before any draw uses it" from "the draws that would use it never reach submission."

Narrower than 7.1/7.2 and only pays off if the answer is one of those two, so it is third.

---

## 8. Observations wanted from the user

Three cheap observations that would each prune a large part of the tree:

1. **Does the HUD / minimap render?** Splits "scene never written" from "scene written, then zeroed
   by exposure" (§6.4).
2. **Is the screen pure black, or very dark with structure?** Raising monitor brightness, or
   checking a screenshot's histogram, answers it. Dark-with-structure means the geometry does reach
   the screen and this is a scaling problem, not a missing-write problem.
3. **In a capture, what writes the display buffer?** A tonemap draw that executes and writes black
   is a very different bug from a tonemap draw that never runs.

---

## 9. Related work on this branch

- Vulkan validation now runs and is non-fatal (commit `33cb277`). Before the Vulkan SDK was
  installed the layer was absent and `--vulkan-validation true` silently disabled itself, so runs
  looked clean because nothing was checking. Run with
  `.\drive_gta3.ps1 -Build windows-nolauncher -ExtraArgs @('--vulkan-validation','true')`.
  Do **not** combine with `--rd`; RenderDoc's hooks and the layer interfere.
- Validation findings still open, none of which explains the black world but all of which are real:
  malformed SPIR-V structured control flow from our own codegen
  (`VUID-VkShaderModuleCreateInfo-pCode-08737`); a vertex/fragment interface mismatch at Location 0
  (`VUID-RuntimeSpirv-OpEntryPoint-08743`); unnormalized-coordinate samplers paired with 3D and
  mipped views (`08609` / `08610` / `09635`); depth/stencil layout mismatches (`09588` / `09590`);
  uncompressed views of block-compressed images spanning all mips
  (`07072` / `09487`).
- Three sampler bugs found by validation and fixed in `33cb277` (LOD-bias clamp, unnormalized-
  coordinate filter equality, inverted min/max LOD).
