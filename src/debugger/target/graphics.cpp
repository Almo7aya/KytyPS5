#include "debugger/target/graphics.h"

#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "debugger/core/session.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <spirv-tools/libspirv.hpp>
#include <unordered_map>

namespace Debugger::Graphics {

namespace {

// The GPU thread writes these and the presentation thread reads them, so both take the lock.
// Neither runs inside a trap handler, which is what makes an ordinary mutex fine here — unlike
// the breakpoint table, which cannot use one.
std::mutex g_mutex;

struct ShaderEntry {
	ShaderSummary         summary;
	std::string           isa;
	std::string           ir;
	std::vector<uint32_t> spirv;
	std::string           spirv_text; // filled the first time it is asked for
};

std::unordered_map<uint64_t, ShaderEntry> g_shaders;
uint32_t                                  g_shader_sequence = 0;

// One frame's worth of draws, capped: a heavy frame can issue tens of thousands, and the list
// exists to be read by a human.
constexpr size_t MAX_DRAWS_PER_FRAME = 4096;

std::vector<DrawRecord> g_current_frame;
std::vector<DrawRecord> g_last_frame;
bool                    g_current_truncated = false;
bool                    g_last_truncated    = false;

uint32_t g_frame                 = 0;
uint32_t g_draws_last_frame      = 0;
uint32_t g_dispatches_last_frame = 0;
uint64_t g_total_draws           = 0;
uint64_t g_total_dispatches      = 0;
uint64_t g_total_flips           = 0;

bool g_logged_first_shader = false;
bool g_logged_first_frame  = false;

bool Disassemble(const std::vector<uint32_t>& spirv, std::string& out) {
	if (spirv.empty()) {
		return false;
	}

	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	return tools.Disassemble(spirv, &out,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT));
}

} // namespace

const char* StageName(ShaderStage stage) {
	switch (stage) {
		case ShaderStage::Vertex: return "vertex";
		case ShaderStage::Pixel: return "pixel";
		case ShaderStage::Compute: return "compute";
		case ShaderStage::Fetch: return "fetch";
		case ShaderStage::Unknown: break;
	}
	return "unknown";
}

const char* KindName(DrawKind kind) {
	switch (kind) {
		case DrawKind::Draw: return "draw";
		case DrawKind::DrawIndexed: return "draw indexed";
		case DrawKind::DrawIndirect: return "draw indirect";
		case DrawKind::Dispatch: return "dispatch";
	}
	return "?";
}

bool IsCapturing() {
	return Session::IsEnabled();
}

bool WantsShaderText() {
	return Session::IsEnabled();
}

void RecordShader(ShaderStage stage, uint64_t hash, uint64_t base_address, uint32_t gcn_bytes,
                  const uint32_t* spirv, size_t spirv_words, const std::string& isa,
                  const std::string& ir) {
	if (!IsCapturing()) {
		return;
	}

	const std::lock_guard lock(g_mutex);

	auto& entry = g_shaders[hash];

	// The same shader is recompiled under different specialisations; keep the first record and
	// only fill in text that was missing, so opening one always shows something.
	if (entry.summary.hash == 0) {
		entry.summary.hash         = hash;
		entry.summary.base_address = base_address;
		entry.summary.stage        = stage;
		entry.summary.gcn_bytes    = gcn_bytes;
		entry.summary.sequence     = g_shader_sequence++;
	}

	entry.summary.spirv_words = static_cast<uint32_t>(spirv_words);

	// One line, the first time only: enough to tell from a log that GPU capture is actually
	// wired up, without narrating every shader in a session that compiles thousands.
	if (!g_logged_first_shader) {
		g_logged_first_shader = true;
		LOGF("Debugger: GPU capture live, first shader is %s hash=0x%016" PRIx64 "\n",
		     StageName(stage), hash);
	}

	if (entry.isa.empty() && !isa.empty()) {
		entry.isa = isa;
	}
	if (entry.ir.empty() && !ir.empty()) {
		entry.ir = ir;
	}
	if (entry.spirv.empty() && spirv != nullptr && spirv_words != 0) {
		entry.spirv.assign(spirv, spirv + spirv_words);
		entry.spirv_text.clear();
	}
}

std::vector<ShaderSummary> Shaders() {
	const std::lock_guard lock(g_mutex);

	std::vector<ShaderSummary> out;
	out.reserve(g_shaders.size());
	for (const auto& [hash, entry]: g_shaders) {
		out.push_back(entry.summary);
	}

	std::sort(out.begin(), out.end(), [](const ShaderSummary& a, const ShaderSummary& b) {
		return a.sequence < b.sequence;
	});
	return out;
}

bool GetShaderCode(uint64_t hash, ShaderCode& out) {
	const std::lock_guard lock(g_mutex);

	const auto it = g_shaders.find(hash);
	if (it == g_shaders.end()) {
		return false;
	}

	if (it->second.spirv_text.empty() && !it->second.spirv.empty()) {
		Disassemble(it->second.spirv, it->second.spirv_text);
	}

	out.isa   = it->second.isa;
	out.ir    = it->second.ir;
	out.spirv = it->second.spirv_text;
	return true;
}

void RecordDraw(const DrawRecord& record) {
	if (!IsCapturing()) {
		return;
	}

	const std::lock_guard lock(g_mutex);

	if (record.kind == DrawKind::Dispatch) {
		g_total_dispatches++;
	} else {
		g_total_draws++;
	}

	if (g_current_frame.size() >= MAX_DRAWS_PER_FRAME) {
		g_current_truncated = true;
		return;
	}

	auto stored  = record;
	stored.frame = g_frame;
	stored.index = static_cast<uint32_t>(g_current_frame.size());
	g_current_frame.push_back(stored);
}

void RecordFlip() {
	if (!IsCapturing()) {
		return;
	}

	const std::lock_guard lock(g_mutex);

	g_total_flips++;
	g_frame++;

	g_draws_last_frame      = 0;
	g_dispatches_last_frame = 0;
	for (const auto& draw: g_current_frame) {
		if (draw.kind == DrawKind::Dispatch) {
			g_dispatches_last_frame++;
		} else {
			g_draws_last_frame++;
		}
	}

	// Only swap in a frame that had something in it, so a flip with no draws does not blank a
	// list somebody is reading.
	if (!g_current_frame.empty()) {
		g_last_frame.swap(g_current_frame);
		g_last_truncated = g_current_truncated;

		if (!g_logged_first_frame) {
			g_logged_first_frame = true;
			LOGF("Debugger: first captured frame has %u draws and %u dispatches\n",
			     g_draws_last_frame, g_dispatches_last_frame);
		}
	}

	g_current_frame.clear();
	g_current_truncated = false;
}

std::vector<DrawRecord> LastFrame() {
	const std::lock_guard lock(g_mutex);
	return g_last_frame;
}

Stats GetStats() {
	const std::lock_guard lock(g_mutex);

	Stats stats {};
	stats.frame                 = g_frame;
	stats.draws_last_frame      = g_draws_last_frame;
	stats.dispatches_last_frame = g_dispatches_last_frame;
	stats.draws_this_frame      = static_cast<uint32_t>(g_current_frame.size());
	stats.total_draws           = g_total_draws;
	stats.total_dispatches      = g_total_dispatches;
	stats.total_flips           = g_total_flips;
	stats.shader_count          = static_cast<uint32_t>(g_shaders.size());
	stats.truncated             = g_last_truncated;
	return stats;
}

bool DumpShader(uint64_t hash, std::string& path_out) {
	ShaderCode code {};
	if (!GetShaderCode(hash, code)) {
		return false;
	}

	const auto folder = Config::GetShaderLogFolder() / "debugger";
	Common::File::CreateDirectories(folder);

	char name[64] {};
	std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));

	const auto write = [&folder, &name](const char* extension, const std::string& text) {
		if (text.empty()) {
			return;
		}
		auto path = folder / (std::string(name) + extension);

		Common::File file;
		file.Create(path);
		if (!file.IsInvalid()) {
			file.Printf("%s", text.c_str());
			file.Close();
		}
	};

	write(".rdna2", code.isa);
	write(".ir", code.ir);
	write(".spvasm", code.spirv);

	path_out = folder.string();
	return true;
}

void Reset() {
	const std::lock_guard lock(g_mutex);

	g_shaders.clear();
	g_shader_sequence = 0;
	g_current_frame.clear();
	g_last_frame.clear();
	g_current_truncated     = false;
	g_last_truncated        = false;
	g_frame                 = 0;
	g_draws_last_frame      = 0;
	g_dispatches_last_frame = 0;
	g_total_draws           = 0;
	g_total_dispatches      = 0;
	g_total_flips           = 0;
	g_logged_first_shader   = false;
	g_logged_first_frame    = false;
}

} // namespace Debugger::Graphics
