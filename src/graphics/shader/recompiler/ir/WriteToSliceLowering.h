#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_WRITETOSLICELOWERING_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_WRITETOSLICELOWERING_H_

#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <string>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Runs a UE "WriteToSlice" export shader as a plain instanced vertex shader.
//
// The pattern is an ES+GS pair where the GS is a strict 1:1 passthrough: the ES writes its outputs to
// the ESGS ring in LDS and exports nothing, and the GS reads them straight back and owns every
// export, adding only a render-target slice index. Nothing in there needs a geometry stage - the pair
// is semantically an instanced vertex shader - so rather than emulating the ring, this drops it: each
// ring store becomes an export of the single component the GS forwarded it to, and the GS is gone.
//
// `plan` says which store becomes which component; it is derived from the two shaders and rejects
// anything that does not fit the pattern exactly (see WriteToSliceAnalysis.h). Fails rather than
// guessing if the plan and the program disagree, since a mismatch would export a wrong position or a
// wrong slice index and both are silent.
bool LowerWriteToSliceExports(Program& program, const WriteToSlice::EsPlan& plan,
                              std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_IR_WRITETOSLICELOWERING_H_ */
