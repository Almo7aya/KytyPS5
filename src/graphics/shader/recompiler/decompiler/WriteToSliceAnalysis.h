#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WRITETOSLICEANALYSIS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WRITETOSLICEANALYSIS_H_

#include "graphics/shader/recompiler/decompiler/ShaderDecoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::WriteToSlice {

// Analysis of a "WriteToSlice" geometry shader: the passthrough GS that UE's
// RasterizeToVolumeTexture uses to fan a fullscreen primitive across the slices of a volume target.
// See docs/gta3-black-world-diagnosis.md Part 9.
//
// The emulator has no geometry-shader stage, but this pattern needs none. The GS neither amplifies
// nor mixes vertices: each output vertex is a pure copy of one input vertex, read straight back out
// of the ESGS ring the ES wrote. So ES+GS together are just an instanced vertex shader, and the ring
// round-trip can be removed rather than emulated.
//
// What this analysis produces is the missing half of that: the map from ESGS ring byte offset to the
// export slot the GS forwards it to. With it, the ES's ring stores can be retargeted directly into
// exports. The map has to be *derived* - it is not the order a struct layout would predict, and the
// GS reshuffles registers before exporting, so reading it off the disassembly by hand is unreliable.
//
// This is a recognizer, not a general GS compiler. Anything that does not fit the passthrough shape
// exactly is rejected, so unrelated geometry shaders keep being skipped as before.
//
// Rejecting is the safe outcome: callers keep skipping the draw, which is today's behaviour. The one
// thing this must never do is return a map it is not sure of - a wrong offset silently produces a
// wrong position or a wrong slice index.
//
// The register tracking is a linear, control-flow-insensitive scan, which is sound here for one
// reason: every value the analysis reports has to have been agreed on by *all* the paths that
// produced it. The GS stages its outputs in a second LDS region once per emitted vertex on separate
// branch paths, and a conflicting store poisons its slot rather than overwriting it, so a
// disagreement between vertices - i.e. anything that is not a uniform passthrough - rejects.

enum class SourceKind : uint8_t {
	Unknown,    // not tracked - forces a reject if an enabled export component depends on it
	RingOffset, // a dword read back from the ESGS ring, at `value` bytes into the vertex
	Immediate,  // a literal the GS materialized itself (e.g. the 0 padding a PARAM export)
};

struct ExportComponent {
	uint32_t   target    = 0;                    // raw export target: 0x0c POS0, 0x0d POS1, 0x20+ PARAM
	uint32_t   component = 0;                    // 0..3
	SourceKind kind      = SourceKind::Unknown;  //
	uint32_t   value     = 0;                    // ring byte offset, or the immediate's bits
};

struct RingMap {
	bool                         matched = false;
	uint32_t                     stride  = 0; // ESGS bytes per vertex
	std::vector<ExportComponent> components;  // one entry per enabled export component
	std::string                  reject_reason;

	[[nodiscard]] bool HasTarget(uint32_t target) const;
};

// Derives the ring-offset -> export map from a decoded passthrough geometry shader. On any deviation
// from the pattern the result has `matched == false` and `reject_reason` set.
RingMap AnalyzePassthroughGs(const Decoder::Program& gs);

// Human-readable dump, for the offline shader_disasm tool and for diagnostics.
std::string RingMapToString(const RingMap& map);

} // namespace Libs::Graphics::ShaderRecompiler::WriteToSlice

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WRITETOSLICEANALYSIS_H_ */
