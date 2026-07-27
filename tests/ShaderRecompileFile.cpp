// Offline shader recompile harness (KYTY_DIAG).
//
// Recompiles a dumped GCN shader binary (uint32 words, e.g. _Shaders/precmp/*.bin written by
// DumpShaderRecompilerPrecmp) through the exact pipeline the emulator uses, so a recompiler
// hang/crash can be reproduced without launching the game.
//
// usage: shader_recompile_file <shader.bin> [ps|vs|cs] [watchdog_seconds]
// exit codes: 0 = compiled ok, 1 = compile error, 2 = bad args/io, 3 = watchdog (hang reproduced)

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shader.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Libs::Graphics;

namespace {

std::atomic<bool> g_done{false};

void WatchdogRun(uint64_t seconds) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (!g_done.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	if (!g_done.load(std::memory_order_relaxed)) {
		std::fprintf(stderr,
		             "WATCHDOG: TryRecompile still running after %" PRIu64
		             " s -- recompiler hang reproduced\n",
		             seconds);
		std::fflush(stderr);
		std::_Exit(3);
	}
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s <shader.bin> [ps|vs|cs] [watchdog_seconds]\n", argv[0]);
		return 2;
	}
	const char* path      = argv[1];
	const char* stage_arg = argc > 2 ? argv[2] : "ps";
	const auto  watchdog_seconds =
	    static_cast<uint64_t>(argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 120);

	FILE* f = std::fopen(path, "rb");
	if (f == nullptr) {
		std::fprintf(stderr, "cannot open %s\n", path);
		return 2;
	}
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size % 4) != 0) {
		std::fprintf(stderr, "bad size %ld (must be a non-zero multiple of 4)\n", size);
		std::fclose(f);
		return 2;
	}
	std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
	if (std::fread(code.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) {
		std::fprintf(stderr, "short read on %s\n", path);
		std::fclose(f);
		return 2;
	}
	std::fclose(f);

	ShaderType stage = ShaderType::Pixel;
	if (std::strcmp(stage_arg, "vs") == 0) {
		stage = ShaderType::Vertex;
	} else if (std::strcmp(stage_arg, "cs") == 0) {
		stage = ShaderType::Compute;
	} else if (std::strcmp(stage_arg, "ps") != 0) {
		std::fprintf(stderr, "unknown stage '%s' (expected ps|vs|cs)\n", stage_arg);
		return 2;
	}

	Common::ThreadsSubsystem::Instance()->Init(nullptr);
	Config::ConfigSubsystem::Instance()->Init(nullptr);
	Config::ConfigOptions config_options;
	config_options.printf_direction = Config::OutputDirection::Console;
	Config::Load(config_options);
	Log::LogSubsystem::Instance()->Init(nullptr);

	// dumpcfg mode: decode + build CFG, print structure, and exit before any hanging phase.
	if (argc > 3 && std::strcmp(argv[3], "dumpcfg") == 0) {
		ShaderRecompiler::Decoder::Program decoded;
		std::string                        decode_error;
		if (!ShaderRecompiler::Decoder::DecodeProgram(code, decoded, &decode_error)) {
			std::fprintf(stderr, "decode failed: %s\n", decode_error.c_str());
			return 1;
		}
		ShaderRecompiler::CFG::Graph cfg;
		std::string                  cfg_error;
		if (!ShaderRecompiler::CFG::BuildGraph(decoded, cfg, &cfg_error)) {
			std::fprintf(stderr, "BuildGraph failed: %s\n", cfg_error.c_str());
			return 1;
		}
		std::string struct_error;
		const bool  structured = ShaderRecompiler::CFG::Structurize(cfg, &struct_error);
		std::fprintf(stderr, "Structurize: %s %s\n", structured ? "OK" : "FAILED",
		             struct_error.c_str());
		std::fprintf(stderr, "%s\n", ShaderRecompiler::CFG::GraphToString(cfg).c_str());
		std::fprintf(stderr, "DECODED:\n%s\n",
		             ShaderRecompiler::Decoder::ProgramToString(decoded).c_str());
		return 0;
	}

	ShaderRecompiler::CompileOptions options;
	options.stage         = stage;
	options.dump_ir       = false;
	options.user_data     = nullptr; // zero/sentinel user SGPRs, read_memory reads zeros
	options.shader_base   = 0;       // falls back to code.data()
	options.shader_hash   = 0;
	options.dump_label    = "ShaderRecompileFile";

	std::fprintf(stderr, "recompiling %s: %zu dwords, stage=%s, watchdog=%" PRIu64 " s\n", path,
	             code.size(), stage_arg, watchdog_seconds);
	std::fflush(stderr);

	std::thread watchdog(WatchdogRun, watchdog_seconds);
	watchdog.detach();

	const auto                     begin = std::chrono::steady_clock::now();
	ShaderRecompiler::CompileResult result;
	std::string                     error;
	const bool ok = ShaderRecompiler::TryRecompile(code, options, result, &error);
	g_done.store(true, std::memory_order_relaxed);

	const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	                            std::chrono::steady_clock::now() - begin)
	                            .count();
	if (!ok) {
		std::fprintf(stderr, "TryRecompile FAILED after %lld ms: %s\n",
		             static_cast<long long>(elapsed_ms), error.c_str());
		return 1;
	}
	std::fprintf(stderr, "TryRecompile OK after %lld ms: spirv_words=%zu\n",
	             static_cast<long long>(elapsed_ms), result.spirv.size());
	return 0;
}
