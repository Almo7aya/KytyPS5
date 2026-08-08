#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WRITETOSLICELOWERING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WRITETOSLICELOWERING_H_

#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

namespace Libs::Graphics::ShaderRecompiler::IR {

struct WriteToSliceLoweringStats {
	uint32_t rewritten_stores = 0;
	uint32_t constant_exports = 0;
};

// Elides the ESGS ring round-trip of a passthrough ES+GS pair. Each of the vertex stage's ring
// stores becomes an export of the single component the geometry stage would have put it in, so the
// pair collapses into the instanced vertex shader it already is semantically.
//
// One hardware export becomes several instructions this way, which is why each carries
// ExportInfo::component_store - without it the second store to a target would reset the components
// the first one wrote back to the export path's defaults.
[[nodiscard]] WriteToSliceLoweringStats LowerWriteToSlice(Program&                         program,
                                                          const Decoder::WriteToSlicePlan& plan);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_WRITETOSLICELOWERING_H_ */
