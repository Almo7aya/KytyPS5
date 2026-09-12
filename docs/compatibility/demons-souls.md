# Demon's Souls rendering profile

This separate change depends on the portable rendering optimizations. It applies
only to title `PPSA01341`, application version `01.007.000`. Other versions and
titles keep the normal synchronization and shader paths. There are no environment
variables or machine-specific configuration files required by this profile.

## Compute boundaries

The checked guest command stream marks dependent compute groups with explicit
synchronization. Within such a group, resource preparation still runs for each
dispatch, but a pending compute dependency may be deferred across consecutive
dispatches. This is a **title-specific assumption**, not proof that arbitrary
consecutive Vulkan dispatches are independent.

Uploads, transfers, draws, host commands, natural submissions and guest
ACQUIRE_MEM, WAIT_REG_MEM, end-of-pipe or synchronization events drain the pending
dependency. Full guest barriers subsume it. Indirect arguments are resolved before
selecting the command buffer or checking the pending dependency. The generic
bounded submission path drains the dependency before submitting as well.

The GPU regression test checks independent writes, RAW/WAR dependencies, GPU
produced indirect counts, transfer boundaries and asynchronous submissions against
all output words. It also checks isolation of unknown titles and versions. These
fixtures do not constitute a full-game compatibility test.

## Linear subset of the periodic-copy kernel

For shader hash `eb7456322124ecc7`, the verified behavior is
`dst[i] = src[i % period]` for `i < count`. Only `period >= count` is lowered to
the existing coherent `BufferCache::CopyBuffer` implementation. The guards require
exact resource formats, addressing controls, dispatch and workgroup shape,
matching bounds, no overlap or physical alias, and CPU-readable clean controls.
All other cases use the original shader. No game shader bytes are included.

This small specialization is kept separate from general shader analysis. A future
IR-based proof could generalize it; the current hash recognizer makes no such claim.
