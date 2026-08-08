#include "graphics/shader/recompiler/ir/WriteToSliceLowering.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

constexpr uint32_t ExportTargetPosition0 = 0x0c;
constexpr uint32_t ExportTargetPosition1 = 0x0d;
constexpr uint32_t ExportTargetParameter = 0x20;

[[nodiscard]] uint32_t RawTarget(Decoder::WriteToSliceTarget target) {
	switch (target) {
		case Decoder::WriteToSliceTarget::Position0: return ExportTargetPosition0;
		case Decoder::WriteToSliceTarget::Position1: return ExportTargetPosition1;
		case Decoder::WriteToSliceTarget::Parameter0: return ExportTargetParameter;
	}
	return ExportTargetPosition0;
}

[[nodiscard]] ExportTargetKind TargetKind(Decoder::WriteToSliceTarget target) {
	return target == Decoder::WriteToSliceTarget::Parameter0 ? ExportTargetKind::Parameter
	                                                         : ExportTargetKind::Position;
}

[[nodiscard]] uint32_t TargetIndex(Decoder::WriteToSliceTarget target) {
	return target == Decoder::WriteToSliceTarget::Position1 ? 1u : 0u;
}

[[nodiscard]] Operand ImmediateOperand(uint32_t value) {
	Operand operand;
	operand.kind = OperandKind::ImmediateU32;
	operand.imm  = value;
	return operand;
}

// The export emitter reads a component out of src[component] and skips the ones the enable mask
// clears, so the leading slots stay Null and are never loaded.
[[nodiscard]] Instruction MakeComponentExport(uint32_t pc, Decoder::WriteToSliceTarget target,
                                              uint32_t component, const Operand& value) {
	Instruction inst;
	inst.pc                          = pc;
	inst.op                          = Opcode::Export;
	inst.dst.kind                    = OperandKind::Null;
	inst.export_info.kind            = TargetKind(target);
	inst.export_info.target          = RawTarget(target);
	inst.export_info.index           = TargetIndex(target);
	inst.export_info.en              = 1u << component;
	inst.export_info.component_store = true;
	inst.src_count                   = component + 1u;
	inst.src[component]              = value;
	return inst;
}

[[nodiscard]] const Decoder::WriteToSliceStore* FindStore(const Decoder::WriteToSlicePlan& plan,
                                                         const Instruction&               inst) {
	if (inst.op != Opcode::DsWriteB32 || inst.memory.kind != ResourceKind::Lds) {
		return nullptr;
	}
	for (const auto& store: plan.stores) {
		if (store.pc == inst.pc && store.ring_offset == inst.memory.offset) {
			return &store;
		}
	}
	return nullptr;
}

} // namespace

WriteToSliceLoweringStats LowerWriteToSlice(Program&                         program,
                                            const Decoder::WriteToSlicePlan& plan) {
	WriteToSliceLoweringStats stats;
	if (!plan.lowerable) {
		return stats;
	}

	bool constants_emitted = false;
	for (auto& block: program.blocks) {
		std::vector<Instruction> lowered;
		lowered.reserve(block.instructions.size() + plan.constants.size());
		for (const auto& inst: block.instructions) {
			const auto* store = FindStore(plan, inst);
			if (store == nullptr) {
				lowered.push_back(inst);
				continue;
			}
			// LowerDsWrite/LowerDsWrite2 both put the stored value in src[0].
			lowered.push_back(
			    MakeComponentExport(inst.pc, store->target, store->component, inst.src[0]));
			stats.rewritten_stores++;

			if (constants_emitted) {
				continue;
			}
			// Components the geometry stage materialized itself rather than reading from the ring.
			// They go beside the first rewritten store so they share its control flow: a lane that
			// holds no vertex must not export at all, and a lane that does must export all of it.
			for (const auto& slot: plan.constants) {
				lowered.push_back(MakeComponentExport(inst.pc, slot.target, slot.component,
				                                      ImmediateOperand(slot.constant)));
				stats.constant_exports++;
			}
			constants_emitted = true;
		}
		block.instructions = std::move(lowered);
	}
	program.write_to_slice_lowered = stats.rewritten_stores != 0;
	return stats;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
