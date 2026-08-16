#ifndef EMULATOR_SRC_DEBUGGER_TARGET_GRAPHICS_H_
#define EMULATOR_SRC_DEBUGGER_TARGET_GRAPHICS_H_

#include "common/common.h"

#include <cstdint>
#include <string>
#include <vector>

// Observation layer for the GPU side of the debugger.
//
// Everything here is read-only: the emulator's graphics code reports what it did, and the
// overlay renders it. Nothing halts or reorders the GPU thread — a PM4 breakpoint has to stop
// that thread between submits, never while it holds the command scheduler, and getting that
// wrong deadlocks presentation along with the debugger that is meant to be showing it. That is
// a separate piece of work; see docs/debugger-design.md.
namespace Debugger::Graphics {

enum class ShaderStage : uint8_t { Unknown, Vertex, Pixel, Compute, Fetch };

const char* StageName(ShaderStage stage);

struct ShaderSummary {
	uint64_t    hash         = 0;
	uint64_t    base_address = 0;
	ShaderStage stage        = ShaderStage::Unknown;
	uint32_t    gcn_bytes    = 0;
	uint32_t    spirv_words  = 0;
	uint32_t    sequence     = 0; // order in which the recompiler first produced it
};

struct ShaderCode {
	std::string isa;   // decoded RDNA2
	std::string ir;    // recompiler IR
	std::string spirv; // SPIR-V disassembly, produced on demand
};

// True while the debugger is enabled, i.e. while any of this should be captured at all.
bool IsCapturing();

// The recompiler only builds its ISA and IR text when asked, because the strings are large.
// The debugger wants them so the shader views have something to show.
bool WantsShaderText();

// Reported by the shader recompiler once per shader it produces.
void RecordShader(ShaderStage stage, uint64_t hash, uint64_t base_address, uint32_t gcn_bytes,
                  const uint32_t* spirv, size_t spirv_words, const std::string& isa,
                  const std::string& ir);

std::vector<ShaderSummary> Shaders();

// SPIR-V is disassembled here rather than at compile time, so the cost is only paid for shaders
// somebody actually opens.
bool GetShaderCode(uint64_t hash, ShaderCode& out);

enum class DrawKind : uint8_t { Draw, DrawIndexed, DrawIndirect, Dispatch };

const char* KindName(DrawKind kind);

struct DrawRecord {
	uint32_t frame      = 0;
	uint32_t index      = 0; // position within the frame
	uint64_t submit_id  = 0;
	DrawKind kind       = DrawKind::Draw;
	uint32_t count      = 0; // vertices or indices
	uint32_t instances  = 0;
	uint32_t groups[3]  = {}; // compute thread groups
	uint64_t vs_address = 0;
	uint64_t ps_address = 0;
	uint64_t cs_address = 0;
};

void RecordDraw(const DrawRecord& record);

// Called when the command processor flips, which is what closes a frame.
void RecordFlip();

// Draws captured for the frame most recently completed, so the list does not churn while it is
// being read.
std::vector<DrawRecord> LastFrame();

struct Stats {
	uint32_t frame                 = 0;
	uint32_t draws_last_frame      = 0;
	uint32_t dispatches_last_frame = 0;
	uint32_t draws_this_frame      = 0;
	uint64_t total_draws           = 0;
	uint64_t total_dispatches      = 0;
	uint64_t total_flips           = 0;
	uint32_t shader_count          = 0;
	bool     truncated             = false; // the per-frame list hit its cap
};

Stats GetStats();

// Write a shader's ISA, IR and SPIR-V next to the other shader dumps. Returns the folder used.
bool DumpShader(uint64_t hash, std::string& path_out);

void Reset();

} // namespace Debugger::Graphics

#endif // EMULATOR_SRC_DEBUGGER_TARGET_GRAPHICS_H_
