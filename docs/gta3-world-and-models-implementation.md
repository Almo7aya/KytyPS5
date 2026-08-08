# World and models: what the fixes are, and why each one works

Study of the seven commits on `origin/gta3-world-and-models-render`, written before reimplementing
them. The point of this document is the *mechanism* of each change — enough that the implementation
can be reproduced from reasoning rather than copied — plus an honest classification of which are
fixes and which are stopgaps.

Three defects hide behind "the world and models do not load". They are independent and were fixed in
this order because each one masks the next.

---

## 1. The world is black — the colour-grading LUT is never written

### Mechanism

Unreal's combined colour grading is a 32×32×32 `A2B10G10R10_UNORM` volume, sampled by the tonemapper
in **logarithmic** scene-colour space. It is produced by `FCombineLUTsPS` through
`RasterizeToVolumeTexture`, which is a real ES+GS pair. `ShouldSkipGeShader` (`renderDraw.cpp:467`)
only admits `stages == 0` or `stages == 0x02002000` (NGG passthrough), so that draw is dropped and
the volume keeps its zero initialisation.

The tonemapper then samples an all-zero LUT, which maps every scene colour to black. The HUD survives
because it is drawn later, straight into the display target.

Confirmed on this tree by a probe at the sampled bind:

```
[lut] addr=0x2034f20000 size=0x00200000 fmt=64 type=10 32x32x32
      levels=1 layers=1 samples=1 bpb=4 tile=27 zero=1
```

`fmt=64` is `A2B10G10R10_UNORM_PACK32`, `type=10` is `kColor3D`, `tile=27` is `kRenderTarget`, and
`zero=1` means every byte of the 2 MiB guest allocation is zero.

### The change

Detect that exact resource and upload a neutral cube in place of the zeros.

`IsEmptyColorGradingLut(info)` matches on format, `kColor3D`, extent 32³, one mip, one layer, one
sample, 4 bytes per texel, and a non-empty guest range whose whole backing reads zero. Note what it
does **not** test: tile mode, and the binding route. The allocation is 2 MiB because each depth slice
takes its own 64 KiB render-target tile, so any comparison against the 128 KiB packed texel size
fails; commit `9ac25cc` exists solely because an earlier version made that comparison and the
fallback never engaged.

The cube itself must not be an RGB identity ramp. The LUT is addressed logarithmically, so the
coordinate is decoded back to linear scene colour first:

```
linear_range = 14, linear_grey = 0.18, log_grey = 444/1023
black  = exp2(-log_grey * linear_range) * linear_grey
linear = max(exp2((encoded - log_grey) * linear_range) * linear_grey - black, 0)
```

then a neutral filmic rolloff `l(2.51l + 0.03) / (l(2.43l + 0.59) + 0.14)` clamped to [0,1], then the
sRGB transfer, then 10-bit pack with alpha 3. Feeding the coordinate straight to a display transform
would crush ordinary scene values toward black — the very failure being fixed.

The substitution replaces the guest upload inside `InitializeImage`'s existing `if (upload)` branch:
build the normal colour transfer plan, upload the generated cube through `UploadTransient`, add the
transient offset to every region, and `image.Upload`. Guest memory is never modified.

### Classification: **stopgap**, and it says so

This substitutes data the guest never produced. It restores correct *brightness*, not the title's
grade. The real fix is to execute the `WriteToSlice` ES+GS pattern so the guest writes its own LUT —
at which point the all-zero predicate stops matching and this path retires itself with no further
work. That is tracked separately; it is a recompiler feature, not a renderer one.

---

## 2. The world is still black except bright specks — the separate-translucency clear

### Mechanism

Unreal composites its separate-translucency target as

```
SceneColor = SceneColor * SeparateTranslucency.a + SeparateTranslucency.rgb
```

and relies on that target starting at `(0,0,0,1)`, so that where nothing translucent was drawn, alpha
1 passes the scene through unchanged.

The guest requests that clear by filling the surface's **DCC metadata** with a byte-replicated clear
code, not by any draw and not through the render-target registers. On GFX9/GFX10 the constant-encoded
codes carry the colour themselves:

| code | colour |
| --- | --- |
| `0x00` | `(0,0,0,0)` |
| `0x40` | `(0,0,0,1)` |
| `0x80` | `(1,1,1,0)` |
| `0xC0` | `(1,1,1,1)` |
| `0x20` | use `CB_COLOR#_CLEAR_WORD0/1` |
| `0xFF` | uncompressed — an initialise or decompress, **not** a clear |

For constant codes the hardware never reads the clear-word registers, which is why they measure zero
for this surface and why chasing them is a dead end.

With no clear at all the target accumulates across frames: RGB adds up while alpha decays
multiplicatively toward zero. Once alpha reaches 0 the composite is `SceneColor * 0 + rgb`, so the
scene is multiplied out and only what was drawn *into* the translucency buffer survives — additively.
On screen that is a black world with a few bright specks: street lamps and vehicle lights at night.

### The change

Three pieces that have to be present together, or the clear never happens.

**Track the DCC allocation as logical clear state.** `ResolveRenderColorTarget` sets
`desc.info.metadata = {rt.dcc_addr.addr, 0}` with kind `Dcc` when `dcc_compression_enable` and a
non-zero DCC address. `FindRenderTarget` copies that onto the image and registers the address in
`m_surface_metas`. `DeleteImage` must erase for *any* metadata kind, not just HTile, now that colour
entries exist. Size 0 is deliberate and legal — `ImageOps::Validate` permits it for `Dcc`, as the
video-out path already does; the clear is keyed on the base address the guest's fill targets.

**Recover the code from the fill.** The fill is a compute dispatch that splats one dword sourced from
a companion read-only buffer, and `TryConsumeComputeMetaClear` consumes that dispatch rather than
executing it — so the value has to be read out before it is discarded. The guest value sits at the
read-only descriptor's **base address**; the index the recompiled shader applies is only Kyty's own
host alignment adjustment on top of guest offset 0. So: scan the non-written buffers, read the first
dword at each base, and take the first one that is byte-replicated. A DCC clear code is
byte-replicated by construction, which is what makes that test sufficient.

> This is the step that punishes over-engineering. Adding further constraints — a single companion
> buffer, a bounded footprint, *every* dword in the descriptor identical, a `resource.read` test —
> all sound defensive and all reject the real fill. The failure is silent: no code recorded, no
> clear, black world. In a path where rejection means "quietly do nothing", strictness is not free.

**Apply it at attachment acquisition.** `AcquireRenderTargets` checks `IsMetaCleared`, reads the
recorded code, and turns a constant-encoded code into a Vulkan attachment clear with that colour;
`0x20` falls back to the clear-word registers; anything else — notably `0xFF` — is not a clear and
the attachment keeps its contents. The state is consumed with `TouchMeta` either way, because the
guest issued the clear once.

`ColorClearF16` exists for the register path only: `R16G16B16A16_SFLOAT` clear words are two half
pairs, and decoding them as packed 8888 would produce nonsense. This title never takes that path
(its code is `0x40`), but the surface format is exactly the one that would be misread.

The CP DMA path (`BufferCache::FillBuffer`) knows the filled value directly and records it too, so a
title that clears metadata by DMA rather than by dispatch is covered by the same decode.

### Classification: **fix**

The value the guest actually wrote is decoded and applied. The two wrong behaviours it replaces —
clearing from unprogrammed registers to `(0,0,0,0)`, or not clearing at all — are both documented
regressions.

---

## 3. Characters and vehicles only appear when the camera is inside them

### Mechanism

`PIXEL_PIPE_STAT_DUMP` (`IT_EVENT_WRITE`, event `0x39`) is the RDNA occlusion-query writeback. The
guest stores a 64-bit counter at one address, renders, stores a second 8 bytes above, then reads
`end - begin` **on the CPU** to decide whether a primitive was visible, dropping it from the next
frame's draw list if not.

`CpOpEventWrite` read only `buffer[0]` and discarded the destination address the packet carries in
`buffer[1..2]`, and `TriggerEvent` lumped `0x38`/`0x39`/`0x3a` into "ignoring unsupported
event_write type". Nothing was ever written, so the guest read stale memory, computed a zero (or
negative) delta, and culled. Unreal marks primitives whose bounds contain the camera as
unconditionally visible, which is why exactly those survived.

### The change

Parse the destination for event `0x39` when the packet is long enough (`KYTY_PM4_LEN >= 4`), pass it
to `TriggerEvent`, make `0x38`/`0x3a` explicit no-ops, and write a value at the dump address.

The value is **derived from the slot address**: `begin` at address X gets `X >> 3`, and `end` at
X + 8 gets `(X + 8) >> 3 == (X >> 3) + 1`. So `end - begin` is exactly 1, positive, and identical
every frame.

The reasoning behind that choice matters more than the arithmetic. A shared monotonically increasing
counter also guarantees a positive delta *at the moment of writing*, but the guest reads the two
slots asynchronously and non-atomically, a frame or two later. It can therefore sample a `begin`
written by a newer event against an `end` written by an older one, get a **negative** delta, and cull
the model for that frame — which is flashing geometry. An address-derived value has no time
component, so the pair is always consistent.

### Classification: **stopgap**

There is no Z-pass counting on the host, so every query reports visible and occlusion culling is
effectively disabled. Worth being precise about the cost: the rendered **image is correct** — nothing
on screen is ever wrongly culled — and what is lost is the guest's optimisation, i.e. frame rate
through over-draw. That is a different and much smaller category of wrong than the LUT substitution,
which invents content.

The real fix is `VK_QUERY_TYPE_OCCLUSION`: a query pool, the draws between the two dumps bracketed
inside the render-pass instance (opened lazily in the draw path, so the bracketing has to live there
rather than at the dump), and the resolved count written back when the fence retires. Note that the
write must go through guest backing rather than `BufferCache::WriteHostMemory`, since the fence path
can already hold the buffer-cache lock.

---

## 4. A supporting fix: POS0 must not share `gl_Position` with POS1–POS3

Only POS0 is the clip-space position. POS1–POS3 carry unmodelled auxiliary exports — clip and cull
distance, point size, render-target array index, viewport index. `OutputVariableForExport` returned
the same `gl_PerVertex` variable for any `Position` export regardless of index, and `CollectOutputs`
registered them all as the stage's position output, so whichever export the shader emitted last
overwrote a valid clip-space position.

The change returns no variable for index != 0 (the caller already treats 0 as "skip this export") and
registers only index 0. Dropping an unmodelled export is safe; corrupting the position is not.

**Classification: fix.** Measured no-op for this title — no shader here exports POS1–POS3 — but it is
correct on its own terms and is a prerequisite for the WriteToSlice work, which routes `POS1.z` to
`gl_Layer`.

---

## File map

| Change | Files |
| --- | --- |
| Neutral grading LUT | `renderer/cache/textureCache.cpp` |
| DCC clear state | `renderer/cache/textureCache.{h,cpp}`, `renderer/colorRenderTarget.cpp`, `renderer/renderDraw.cpp` |
| DCC clear codes | `renderer/image/imageInfo.h`, `renderer/renderCompute.cpp`, `renderer/cache/bufferCache.cpp`, plus the four above |
| Occlusion result write | `guest_gpu/command_processor/pm4Handlers.cpp`, `command_processor/commandProcessor.h`, `guest_gpu/graphicsRun.cpp` |
| POS0 isolation | `shader/recompiler/emitter/spirvEmitterAnalysis.cpp`, `shader/recompiler/ir/ShaderInfoCollection.cpp` |

Roughly 280 lines total. Every one of these is small; the difficulty was never the code volume but
identifying which register, which packet dword, or which metadata byte carried the information.

---

## Appendix: the WriteToSlice pair, verified from ISA

Dumped with `ShaderDumpSkippedGeShader` and disassembled with the `shader_disasm` target. Everything
below is read out of `es_0000002008ab0000.bin` / `gs_0000002008a90000.bin`, not inferred.

**Ring map.** The GS does not export the ring values directly - it relocates them through an NGG
vertex-compaction round-trip first, so the map has to be traced end to end:

```
ES writes          GS compaction        GS reads back      GS exports
ring+0,4    ─────────────────────────→  0x720+0,4  ──→ exp 0x20 en=0xf  v4,v5,v6,v6
ring+8,12   ─────────────────────────→  0x728+0,4  ──→ exp 0x0c en=0xf  v0,v1,v2,v3
ring+16,20  ─────────────────────────→  0x730+0,4  ──↗
ring+24     ─────────────────────────→  0x738      ──→ exp 0x0d en=0x4  v0,v0,v7,v0
```

| ring offset | what the ES stores | export |
| --- | --- | --- |
| 0, 4 | `v_mad_f32` pair scale-biased by `UVScaleBias` (`s_buffer_load_dwordx4 s28, s8, offset=16`) | `PARAM0.x, .y` |
| 8, 12 | raw `buffer_load_format_xy v10, v8, s8, s18` | `POS0.x, .y` |
| 16, 20 | `v_mov_b32 v10, 0` / `v_mov_b32 v11, 1.0` | `POS0.z, .w` |
| 24 | `v_add_nc_u32 v9, s16, v8` = `MinZ + InstanceID` | `POS1.z` → `gl_Layer` |

Stride 28 confirmed by `v_mul_u32_u24 v5, 28, v12`. `PARAM0.z/.w` are zero from `v_mov_b32 v6, 0`.

**This confirms Part 10 of the black-world doc and refutes Part 9**, which read the `v_mad` as the
position remap when it is the UV. Implementing from Part 9's table would swap `POS0` and `PARAM0` -
valid SPIR-V, wrong image, and no error anywhere.

**The `s3` gate is present and mandatory.** The ES opens with `s_lshl_b32 vcc_lo, s3, 16` /
`s_bfe_u64 exec_lo, -1, vcc_lo` / `s_cbranch_execz`. `s3` is NGG merged-wave info; Kyty's user data
starts at s8, so s0-s7 are zero, EXEC resolves empty and every store is skipped. Without seeding it
the whole feature is a silent no-op.

**TTMP does not appear** in either shader. Part 8 of the black-world doc called it "the real
blocker"; it is not one.

**The GS is NGG-native.** `s_sendmsg 0x9` (GS_ALLOC_REQ), `exp target=0x14` (primitive export), and
vertex exports guarded by `v_cmpx_gt_u32 exec_lo, s1, v18`. Those are allocation and connectivity
mechanics with no meaning once the pair is lowered to an instanced vertex shader, so they are
dropped - and the classifier must require exactly this shape so another title's GS is never mangled.

### Two traps recorded before they are hit

**`DecodeProgram` returns false for these shaders, and that is meaningless.** The AGC shader size
covers code *plus* `s_code_end` padding *plus* a trailing metadata blob, and the decoder walks the
whole buffer linearly. For the ES: 101 dwords of code ending at `s_setpc_b64 s6` (`0x194`), then 12
`s_code_end`, then 2 zero dwords, then 68 dwords of metadata that decode as garbage. Gating the
classifier or the lowering on that return value - the obvious thing to write - rejects this pair for
a completely spurious reason. The program ends at the first `s_endpgm` or `s_setpc_b64`.

**A DS store has no `vdst`.** The decoder fills `Instruction::dst` from the vdst field
unconditionally, so those bits read as register 0 and every `ds_write` looks like it defines `v0`.
Any provenance tracking that treats `dst` as a definition will lose the value the GS carries in `v0`
across the compaction stores - which is exactly where `PARAM0` loses its origin.

## What is left

Two stopgaps, each with a known real replacement:

1. **WriteToSlice** — execute the ES+GS pattern so the guest produces its own grading LUT. The pair
   is a strict 1:1 passthrough, so it lowers to an instanced vertex shader with `POS1.z → gl_Layer`;
   the ESGS ring round-trip can be elided rather than emulated. Retires the LUT substitution
   automatically.
2. **Real occlusion queries** — restores culling, and with it the frame rate lost to over-draw.

Neither blocks the world or the models rendering correctly.
