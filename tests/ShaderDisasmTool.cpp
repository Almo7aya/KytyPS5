// Offline disassembler for raw guest shader words.
//
// The draw path declines to compile some ES/GS pairs, and a pair that is never compiled is also
// never disassembled - so the reason it is unsupported can only be inferred from register bits.
// ShaderDumpSkippedGeShader writes the raw words of such a pair; this turns them into ISA text.
//
// It links only the decompiler and drives the emulator's own Decoder, so the output is exactly what
// the recompiler would see rather than a second opinion from an external disassembler. Doing this
// offline also means a decode failure costs a message instead of taking the title down, which is why
// the in-process dumper never calls the decoder itself.
//
//   shader_disasm <file.bin> [more.bin ...]
//   shader_disasm --writetoslice <es.bin> <gs.bin>

#include "graphics/shader/recompiler/decompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool ReadWords(const char* path, std::vector<uint32_t>& words) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		std::printf("cannot open %s\n", path);
		return false;
	}
	const auto size = static_cast<std::streamoff>(file.tellg());
	if (size <= 0 || size % static_cast<std::streamoff>(sizeof(uint32_t)) != 0) {
		std::printf("%s is not a whole number of dwords (%lld bytes)\n", path,
		            static_cast<long long>(size));
		return false;
	}
	file.seekg(0);
	words.resize(static_cast<size_t>(size) / sizeof(uint32_t));
	return static_cast<bool>(
	    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size)));
}

int Disassemble(const char* path) {
	std::vector<uint32_t> words;
	if (!ReadWords(path, words)) {
		return 1;
	}

	namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

	Decoder::Program program;
	std::string      error;
	const bool       decoded = Decoder::DecodeProgram(words, program, &error);

	std::printf("; %s: %zu dwords, %zu instructions%s\n", path, words.size(),
	            program.instructions.size(), decoded ? "" : " (decode reported a failure)");
	if (!decoded && !error.empty()) {
		std::printf("; decode error: %s\n", error.c_str());
	}
	// The ring-offset -> export map, derived from this shader rather than assumed. Printing it here
	// means it can be checked against real ISA before any lowering is written against it.
	std::printf("%s", Decoder::WriteToSliceMapToString(Decoder::AnalyzePassthroughGs(program)).c_str());

	// Print whatever was decoded even on failure: a program that stops part way still localizes the
	// instruction that stopped it, which is the thing worth seeing.
	std::printf("%s\n", Decoder::ProgramToString(program).c_str());

	// The decode result is deliberately not returned as a status. The AGC shader size covers
	// s_code_end padding and a trailing metadata blob, so decoding always walks off the end of a
	// perfectly good program and reports a failure.
	return 0;
}

// The pair is what the lowering consumes, and only the pair can be checked: the geometry stage says
// where each ring dword lands, and the vertex stage says which store puts it there. Either alone
// looks fine while the two disagree.
int PlanPair(const char* es_path, const char* gs_path) {
	std::vector<uint32_t> es_words;
	std::vector<uint32_t> gs_words;
	if (!ReadWords(es_path, es_words) || !ReadWords(gs_path, gs_words)) {
		return 1;
	}

	namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

	Decoder::Program es;
	Decoder::Program gs;
	// The decode result is deliberately ignored here for the reason given in Disassemble.
	Decoder::DecodeProgram(es_words, es, nullptr);
	Decoder::DecodeProgram(gs_words, gs, nullptr);

	std::printf("; es %s, gs %s\n", es_path, gs_path);
	const auto map = Decoder::AnalyzePassthroughGs(gs);
	std::printf("%s", Decoder::WriteToSliceMapToString(map).c_str());
	std::printf("%s", Decoder::WriteToSlicePlanToString(Decoder::PlanWriteToSlice(es, map)).c_str());
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc >= 2 && std::string(argv[1]) == "--writetoslice") {
		if (argc != 4) {
			std::printf("usage: shader_disasm --writetoslice <es.bin> <gs.bin>\n");
			return 2;
		}
		return PlanPair(argv[2], argv[3]);
	}
	if (argc < 2) {
		std::printf("usage: shader_disasm <file.bin> [more.bin ...]\n"
		            "       shader_disasm --writetoslice <es.bin> <gs.bin>\n");
		return 2;
	}
	int status = 0;
	for (int i = 1; i < argc; i++) {
		status |= Disassemble(argv[i]);
	}
	return status;
}
