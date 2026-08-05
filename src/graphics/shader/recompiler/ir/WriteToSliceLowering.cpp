#include "graphics/shader/recompiler/ir/WriteToSliceLowering.h"

#include <fmt/format.h>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

[[nodiscard]] ExportTargetKind TargetKind(uint32_t target) {
	if (target == 0x0cu || target == 0x0du) {
		return ExportTargetKind::Position;
	}
	return target >= 0x20u ? ExportTargetKind::Parameter : ExportTargetKind::Unknown;
}

[[nodiscard]] uint32_t TargetIndex(uint32_t target) {
	return target >= 0x20u ? target - 0x20u : target - 0x0cu;
}

// Turns one instruction into an export of a single component, moving `value` into the component slot
// the emitter reads it from.
// `value` is taken by value on purpose: callers pass one of `inst`'s own operands, and this clears
// `inst` first.
void MakeComponentExport(Instruction& inst, uint32_t target, uint32_t component, Operand value) {
	const auto pc = inst.pc;
	inst          = {};
	inst.pc       = pc;
	inst.op       = Opcode::Export;
	inst.dst.kind = OperandKind::Null;
	// The emitter indexes sources by component, so all four slots exist and only the enabled one is
	// read. The rest are zero immediates rather than junk, so a dump reads cleanly.
	inst.src_count            = 4;
	inst.src[component]       = std::move(value);
	inst.export_info.kind     = TargetKind(target);
	inst.export_info.target   = target;
	inst.export_info.index    = TargetIndex(target);
	inst.export_info.en       = 1u << component;
	inst.export_info.component_store = true;
	for (uint32_t i = 0; i < 4u; i++) {
		if (i != component) {
			inst.src[i].kind = OperandKind::ImmediateU32;
			inst.src[i].imm  = 0;
		}
	}
}

} // namespace

bool LowerWriteToSliceExports(Program& program, const WriteToSlice::EsPlan& plan,
                              std::string* error) {
	const auto fail = [error](std::string message) {
		if (error != nullptr) {
			*error = std::move(message);
		}
		return false;
	};

	if (!plan.matched) {
		return fail("WriteToSlice lowering was asked for without a matched plan");
	}
	if (program.stage != ShaderType::Vertex) {
		return fail("WriteToSlice lowering only applies to the vertex stage");
	}
	if (program.blocks.empty()) {
		return fail("WriteToSlice lowering needs at least one block");
	}

	// Keyed the way LowerDsWrite/LowerDsWrite2 leave the stores in the IR: one DsWriteB32 per dword,
	// each keeping the decoder pc and carrying its own byte offset.
	std::unordered_map<uint64_t, const WriteToSlice::RingStore*> wanted;
	for (const auto& store: plan.stores) {
		wanted.emplace((static_cast<uint64_t>(store.pc) << 32u) | store.offset, &store);
	}

	uint32_t retargeted = 0;
	for (auto& block: program.blocks) {
		for (auto& inst: block.instructions) {
			if (inst.op != Opcode::DsWriteB32 || inst.memory.kind != ResourceKind::Lds) {
				continue;
			}
			const auto key = (static_cast<uint64_t>(inst.pc) << 32u) | inst.memory.offset;
			const auto it  = wanted.find(key);
			if (it == wanted.end()) {
				// The plan requires the export shader to use LDS for nothing but ring dwords the GS
				// forwards, so anything left here means the plan does not describe this program.
				return fail(fmt::format(
				    "WriteToSlice lowering left an LDS store at pc 0x{:x} offset {} unaccounted for",
				    inst.pc, inst.memory.offset));
			}
			MakeComponentExport(inst, it->second->target, it->second->component, inst.src[0]);
			retargeted++;
		}
	}

	if (retargeted != plan.stores.size()) {
		return fail(fmt::format("WriteToSlice lowering retargeted {} of {} planned ring stores",
		                        retargeted, plan.stores.size()));
	}

	// Components the GS materialized itself rather than forwarding. They go in the entry block, ahead
	// of everything, so they cannot overwrite a real store.
	std::vector<Instruction> constants;
	constants.reserve(plan.constants.size());
	for (const auto& c: plan.constants) {
		Operand value {};
		value.kind = OperandKind::ImmediateU32;
		value.imm  = c.value;
		Instruction inst {};
		inst.pc = program.blocks.front().start_pc;
		MakeComponentExport(inst, c.target, c.component, value);
		constants.push_back(inst);
	}
	auto& entry = program.blocks.front().instructions;
	entry.insert(entry.begin(), constants.begin(), constants.end());

	program.write_to_slice_lowered = true;
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
