// Offline GCN/RDNA disassembler for raw guest shader dumps.
//
// The renderer skips some draws before anything compiles their shaders (see ShouldSkipGeShader),
// so those shaders never reach the recompiler and never get dumped as text. This tool takes the raw
// guest words written by ShaderDumpSkippedGeShader and prints them, using the emulator's own
// decoder so the output matches what the recompiler would see.
//
// Usage: shader_disasm <file.bin> [...]
//        shader_disasm --writetoslice <es.bin> <gs.bin>
//
// The second form derives the passthrough GS's ESGS ring -> export map and then checks the ES
// against it, printing the store retargeting plan the recompiler will apply. That is the whole
// WriteToSlice analysis, verifiable without launching the game.

#include "graphics/shader/recompiler/decompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool ReadWords(const char* path, std::vector<uint32_t>& words) {
	FILE* f = fopen(path, "rb");
	if (f == nullptr) {
		fprintf(stderr, "cannot open %s\n", path);
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size % 4) != 0) {
		fprintf(stderr, "%s: size %ld is not a whole number of dwords\n", path, size);
		fclose(f);
		return false;
	}
	words.resize(static_cast<size_t>(size) / 4u);
	const size_t read = fread(words.data(), 4, words.size(), f);
	fclose(f);
	if (read != words.size()) {
		fprintf(stderr, "%s: short read\n", path);
		return false;
	}
	return true;
}

using namespace Libs::Graphics::ShaderRecompiler;

// A decode failure still leaves everything decoded so far in `program`, and unsupported instructions
// are recorded rather than fatal, so the caller can print whatever was recovered either way.
bool Decode(const char* path, std::vector<uint32_t>& words, Decoder::Program& program,
            bool return_ends_program) {
	if (!ReadWords(path, words)) {
		return false;
	}
	std::string error;
	const bool  ok =
	    Decoder::DecodeProgram(words, program, &error, {.return_ends_program = return_ends_program});
	if (!ok) {
		printf("!! %s: decode stopped: %s\n", path, error.c_str());
	}
	return ok;
}

int RunWriteToSlice(const char* es_path, const char* gs_path) {
	std::vector<uint32_t> gs_words;
	Decoder::Program      gs {};
	if (!Decode(gs_path, gs_words, gs, false)) {
		return 1;
	}
	const auto map = WriteToSlice::AnalyzePassthroughGs(gs);
	printf("%s", WriteToSlice::RingMapToString(map).c_str());

	// The export shader has no `s_endpgm`: it returns to the merged shader with `s_setpc_b64`.
	std::vector<uint32_t> es_words;
	Decoder::Program      es {};
	if (!Decode(es_path, es_words, es, true)) {
		return 1;
	}
	const auto plan = WriteToSlice::PlanEsExports(es, map);
	printf("%s", WriteToSlice::EsPlanToString(plan).c_str());
	return plan.matched ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.bin> [...]\n       %s --writetoslice <es.bin> <gs.bin>\n",
		        argv[0], argv[0]);
		return 2;
	}

	if (std::string(argv[1]) == "--writetoslice") {
		if (argc != 4) {
			fprintf(stderr, "usage: %s --writetoslice <es.bin> <gs.bin>\n", argv[0]);
			return 2;
		}
		return RunWriteToSlice(argv[2], argv[3]);
	}

	int failures = 0;
	for (int i = 1; i < argc; i++) {
		std::vector<uint32_t> words;
		if (!ReadWords(argv[i], words)) {
			failures++;
			continue;
		}

		printf("======== %s (%zu dwords) ========\n", argv[i], words.size());

		Decoder::Program program {};
		std::string      error;
		// A decode failure still leaves everything decoded so far in `program`, and unsupported
		// instructions are recorded rather than fatal - print whatever we got either way.
		const bool ok = Decoder::DecodeProgram(words, program, &error);
		printf("%s", Decoder::ProgramToString(program).c_str());
		if (!ok) {
			printf("!! decode stopped: %s\n", error.c_str());
			failures++;
		}

		uint32_t unsupported = 0;
		for (const auto& inst : program.instructions) {
			if (inst.opcode == Decoder::Opcode::Unsupported ||
			    inst.opcode == Decoder::Opcode::Unknown) {
				if (unsupported == 0) {
					printf("---- unsupported/unknown ----\n");
				}
				unsupported++;
				printf("  pc=%04x word=0x%08x family=%s op=%u %s\n", inst.pc, inst.word,
				       Decoder::FamilyToString(inst.family).c_str(), inst.opcode_id,
				       inst.unsupported_reason.c_str());
			}
		}
		printf("---- %zu instructions, %u unsupported ----\n", program.instructions.size(),
		       unsupported);

		// If this is a passthrough WriteToSlice geometry shader, report the ESGS ring offset ->
		// export map that the ES store retargeting needs.
		printf("%s\n", WriteToSlice::RingMapToString(WriteToSlice::AnalyzePassthroughGs(program))
		                   .c_str());
	}

	return failures == 0 ? 0 : 1;
}
