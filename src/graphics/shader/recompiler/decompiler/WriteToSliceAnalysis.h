#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_WRITETOSLICEANALYSIS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_WRITETOSLICEANALYSIS_H_

#include "graphics/shader/recompiler/decompiler/ShaderDecoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

// Unreal rasterizes into a volume by drawing one instance per slice through an ES+GS pair whose
// geometry stage is a strict 1:1 passthrough: it reads each vertex back out of the ESGS ring and
// exports it unchanged, emitting the slice index as the render-target array index. Such a pair is
// semantically an instanced vertex shader, so the ring round-trip can be elided rather than
// emulated - but only once it is known which ring dword feeds which export component.
//
// That map cannot be assumed. The obvious reading of the vertex stage (a scale-biased pair must be
// the position remap) is wrong for this pattern - it is the UV - so the map is derived here from the
// geometry stage's own exports, which are the authority on where each ring dword lands.

enum class WriteToSliceTarget : uint8_t { Position0, Position1, Parameter0 };

// An exported component either carries a dword the vertex stage put in the ring, or a constant the
// geometry stage materialized itself. Both have to be reproduced: emitting only the ring-derived
// components would leave the rest at whatever the export path happens to default to.
struct WriteToSliceSlot {
	WriteToSliceTarget target      = WriteToSliceTarget::Position0;
	uint32_t           component   = 0;     // 0..3
	bool               from_ring   = true;
	uint32_t           ring_offset = 0;     // byte offset within one ESGS ring vertex
	uint32_t           constant    = 0;     // raw bits, when from_ring is false
};

struct WriteToSliceMap {
	bool                          lowerable   = false;
	uint32_t                      ring_stride = 0;
	std::vector<WriteToSliceSlot> slots;
	std::string                   reject_reason;
};

// The vertex stage's side of the same pattern: which of its ring stores produces which export
// component. Identified by the store's pc and byte offset, because those are what survive into the
// IR - LowerDsWrite2 splits a paired store into single-dword stores that each keep the pc of the
// encoded instruction and carry their byte offset in MemoryInfo::offset. Matching on that pair means
// the lowering never has to re-derive addresses in IR terms.
struct WriteToSliceStore {
	uint32_t           pc          = 0;
	uint32_t           ring_offset = 0;
	WriteToSliceTarget target      = WriteToSliceTarget::Position0;
	uint32_t           component   = 0;
};

struct WriteToSlicePlan {
	bool                           lowerable   = false;
	uint32_t                       ring_stride = 0;
	std::vector<WriteToSliceStore> stores;
	std::vector<WriteToSliceSlot>  constants; // the components the geometry stage materialized itself
	std::string                    reject_reason;
};

// Derives the ring-offset -> export-component map for a passthrough geometry shader, or reports why
// the shader is not one. Never asserts: an unrecognised shader is a rejection, not a failure.
[[nodiscard]] WriteToSliceMap AnalyzePassthroughGs(const Program& gs);

// Pairs that map with the vertex stage's own ring stores. Rejects unless every ring-fed export
// component has a store behind it - an export left unwritten would read whatever the output variable
// happened to hold, which is a silently wrong image rather than a visible failure.
[[nodiscard]] WriteToSlicePlan PlanWriteToSlice(const Program& es, const WriteToSliceMap& gs_map);

[[nodiscard]] std::string WriteToSliceMapToString(const WriteToSliceMap& map);
[[nodiscard]] std::string WriteToSlicePlanToString(const WriteToSlicePlan& plan);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_DECOMPILER_WRITETOSLICEANALYSIS_H_
