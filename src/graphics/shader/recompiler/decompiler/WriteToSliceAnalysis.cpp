#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

namespace {

constexpr uint32_t ExportTargetPosition0 = 0x0c;
constexpr uint32_t ExportTargetPosition1 = 0x0d;
constexpr uint32_t ExportTargetParameter = 0x20;

constexpr uint32_t MaxVectorRegisters = 256;

// The AGC shader size covers the code, the s_code_end padding the compiler emits so the instruction
// prefetcher can run past the end, and a trailing metadata blob. Decoding walks all of it, so the
// tail is always garbage and the decoder always reports a failure. Stop at the first terminator
// instead - and never treat the decoder's overall result as a verdict on the shader.
size_t ProgramEnd(const Program& gs) {
	for (size_t i = 0; i < gs.instructions.size(); i++) {
		const auto opcode = gs.instructions[i].opcode;
		if (opcode == Opcode::SEndpgm || opcode == Opcode::SSetpcB64) {
			return i + 1;
		}
	}
	return gs.instructions.size();
}

[[nodiscard]] bool IsVectorRegister(const Operand& operand) {
	return operand.kind == OperandKind::Vgpr && operand.reg < MaxVectorRegisters;
}

[[nodiscard]] std::optional<uint32_t> InlineConstant(const Operand& operand) {
	if (operand.kind == OperandKind::IntegerInlineConstant ||
	    operand.kind == OperandKind::LiteralConstant) {
		return operand.value;
	}
	return {};
}

// An LDS address in this pattern is always `stride * vertex_index + constant`. Only the constant
// distinguishes one slot from another: the index selects which vertex a lane is handling, and for a
// passthrough shader every vertex carries the same attribute layout, so the map being derived is the
// same whichever vertex a lane happens to hold. Tracking the constant alone is therefore sound here,
// and it is what lets a linear scan follow the value across the vertex-compaction round-trip.
struct AddressValue {
	uint32_t constant = 0;
	bool     scaled   = false; // built from `stride * something`
};

class Tracker {
public:
	explicit Tracker(uint32_t stride): m_stride(stride) {}

	void SetAddress(uint32_t reg, AddressValue value) {
		if (reg < MaxVectorRegisters) {
			m_address[reg] = value;
			m_ring.erase(reg);
		}
	}

	void SetRingOrigin(uint32_t reg, uint32_t ring_offset) {
		if (reg < MaxVectorRegisters) {
			m_ring[reg] = ring_offset;
			m_address.erase(reg);
		}
	}

	void SetConstant(uint32_t reg, uint32_t value) {
		if (reg < MaxVectorRegisters) {
			m_constant[reg] = value;
			m_address.erase(reg);
			m_ring.erase(reg);
		}
	}

	void Kill(uint32_t reg) {
		if (reg < MaxVectorRegisters) {
			m_address.erase(reg);
			m_ring.erase(reg);
			m_constant.erase(reg);
		}
	}

	[[nodiscard]] std::optional<uint32_t> Constant(uint32_t reg) const {
		const auto found = m_constant.find(reg);
		return found == m_constant.end() ? std::optional<uint32_t> {} : found->second;
	}

	[[nodiscard]] std::optional<AddressValue> Address(uint32_t reg) const {
		const auto found = m_address.find(reg);
		return found == m_address.end() ? std::optional<AddressValue> {} : found->second;
	}

	[[nodiscard]] std::optional<uint32_t> RingOrigin(uint32_t reg) const {
		const auto found = m_ring.find(reg);
		return found == m_ring.end() ? std::optional<uint32_t> {} : found->second;
	}

	// A relocation slot holds whichever ring dword was last written to it. Conflicting writes poison
	// the slot rather than overwriting it: a value survives only if every path that wrote it agreed,
	// so a staging copy that is not uniform across the emitted vertices rejects instead of lying.
	void StoreSlot(uint32_t byte_offset, std::optional<uint32_t> ring_offset) {
		const auto found = m_slot.find(byte_offset);
		if (found == m_slot.end()) {
			m_slot[byte_offset] = ring_offset;
			return;
		}
		if (found->second != ring_offset) {
			found->second.reset();
		}
	}

	[[nodiscard]] std::optional<uint32_t> LoadSlot(uint32_t byte_offset) const {
		const auto found = m_slot.find(byte_offset);
		return found == m_slot.end() ? std::optional<uint32_t> {} : found->second;
	}

	[[nodiscard]] uint32_t Stride() const { return m_stride; }

private:
	uint32_t                                       m_stride;
	std::map<uint32_t, AddressValue>            m_address;
	std::map<uint32_t, uint32_t>                m_ring;
	std::map<uint32_t, uint32_t>                m_constant;
	std::map<uint32_t, std::optional<uint32_t>> m_slot;
};

// A DS store has no vdst, but the decoder fills Instruction::dst from those bits unconditionally, so
// every ds_write looks like it defines v0. Exports define nothing either. Treating either as a
// definition silently drops the provenance a shader carries in v0 across the compaction stores,
// which is precisely where the parameter export loses its origin.
[[nodiscard]] bool DefinesDestination(const Instruction& inst) {
	switch (inst.opcode) {
		case Opcode::DsWriteB32:
		case Opcode::DsWrite2B32:
		case Opcode::Exp: return false;
		default: return true;
	}
}

[[nodiscard]] uint32_t DeriveRingStride(const Program& gs, size_t end) {
	// The ring index is always formed as `stride * vertex`, and the stride is the vertex size in
	// bytes. Take it from the first such multiply rather than assuming a value.
	for (size_t i = 0; i < end; i++) {
		const auto& inst = gs.instructions[i];
		if (inst.opcode != Opcode::VMulU32U24 && inst.opcode != Opcode::VMadU32U24) {
			continue;
		}
		if (const auto constant = InlineConstant(inst.src0); constant && *constant != 0) {
			return *constant;
		}
	}
	return 0;
}

// The vertex stage addresses the ring exactly as the geometry stage addresses its relocation slots,
// so both scans need the same `stride * index + constant` tracking. Only the ring scan cares where a
// value came from, so that part stays with it and this handles addresses alone.
void TrackAddress(const Instruction& inst, uint32_t stride, Tracker& tracker) {
	switch (inst.opcode) {
		case Opcode::VMulU32U24:
		case Opcode::VMadU32U24: {
			const auto scale = InlineConstant(inst.src0);
			if (IsVectorRegister(inst.dst) && scale && *scale == stride) {
				const auto bias =
				    inst.opcode == Opcode::VMadU32U24 ? InlineConstant(inst.src2).value_or(0) : 0;
				tracker.SetAddress(inst.dst.reg, {bias, true});
				return;
			}
			break;
		}
		case Opcode::VAddNcU32: {
			if (IsVectorRegister(inst.dst) && IsVectorRegister(inst.src1)) {
				if (const auto base = tracker.Address(inst.src1.reg)) {
					if (const auto bias = InlineConstant(inst.src0)) {
						tracker.SetAddress(inst.dst.reg, {base->constant + *bias, base->scaled});
						return;
					}
				}
			}
			break;
		}
		default: break;
	}

	if (DefinesDestination(inst)) {
		if (IsVectorRegister(inst.dst)) {
			tracker.Kill(inst.dst.reg);
		}
		if (IsVectorRegister(inst.dst2)) {
			tracker.Kill(inst.dst2.reg);
		}
	}
}

[[nodiscard]] const char* TargetName(WriteToSliceTarget target) {
	switch (target) {
		case WriteToSliceTarget::Position0: return "POS0";
		case WriteToSliceTarget::Position1: return "POS1";
		case WriteToSliceTarget::Parameter0: return "PARAM0";
	}
	return "?";
}

} // namespace

WriteToSliceMap AnalyzePassthroughGs(const Program& gs) {
	WriteToSliceMap map;

	const auto end = ProgramEnd(gs);
	if (end == 0) {
		map.reject_reason = "geometry shader has no terminator";
		return map;
	}

	map.ring_stride = DeriveRingStride(gs, end);
	if (map.ring_stride == 0) {
		map.reject_reason = "no ESGS ring stride could be derived";
		return map;
	}

	Tracker tracker(map.ring_stride);
	// The ring is what the vertex stage wrote before this shader ran, so a load from an address whose
	// constant is below one vertex stride, and which nothing here has stored to, comes straight from
	// the ring.
	const auto loaded_origin = [&tracker](uint32_t byte_offset) -> std::optional<uint32_t> {
		if (const auto slot = tracker.LoadSlot(byte_offset)) {
			return slot;
		}
		return byte_offset < tracker.Stride() ? std::optional<uint32_t> {byte_offset}
		                                     : std::optional<uint32_t> {};
	};

	for (size_t i = 0; i < end; i++) {
		const auto& inst = gs.instructions[i];

		switch (inst.opcode) {
			case Opcode::VMovB32: {
				// The geometry stage materializes some export components itself rather than reading
				// them from the ring, so those constants are part of the map too.
				if (IsVectorRegister(inst.dst)) {
					if (const auto constant = InlineConstant(inst.src0)) {
						tracker.SetConstant(inst.dst.reg, *constant);
						continue;
					}
				}
				break;
			}
			case Opcode::DsReadB32:
			case Opcode::DsRead2B32: {
				if (!IsVectorRegister(inst.dst) || !IsVectorRegister(inst.src0)) {
					break;
				}
				const auto base = tracker.Address(inst.src0.reg);
				if (!base || !base->scaled) {
					break;
				}
				const auto first = loaded_origin(base->constant + inst.offset);
				if (first) {
					tracker.SetRingOrigin(inst.dst.reg, *first);
				} else {
					tracker.Kill(inst.dst.reg);
				}
				if (inst.opcode == Opcode::DsRead2B32) {
					const auto second = loaded_origin(base->constant + inst.secondary_offset);
					if (second) {
						tracker.SetRingOrigin(inst.dst.reg + 1u, *second);
					} else {
						tracker.Kill(inst.dst.reg + 1u);
					}
				}
				continue;
			}
			case Opcode::DsWriteB32:
			case Opcode::DsWrite2B32: {
				if (!IsVectorRegister(inst.src0)) {
					break;
				}
				const auto base = tracker.Address(inst.src0.reg);
				if (!base || !base->scaled) {
					break;
				}
				if (IsVectorRegister(inst.src1)) {
					tracker.StoreSlot(base->constant + inst.offset,
					                  tracker.RingOrigin(inst.src1.reg));
				}
				if (inst.opcode == Opcode::DsWrite2B32 && IsVectorRegister(inst.src2)) {
					tracker.StoreSlot(base->constant + inst.secondary_offset,
					                  tracker.RingOrigin(inst.src2.reg));
				}
				continue;
			}
			case Opcode::Exp: {
				const std::array<const Operand*, 4> operands {&inst.src0, &inst.src1, &inst.src2,
				                                             &inst.src3};
				WriteToSliceTarget                  target = WriteToSliceTarget::Position0;
				switch (inst.exp.target) {
					case ExportTargetPosition0: target = WriteToSliceTarget::Position0; break;
					case ExportTargetPosition1: target = WriteToSliceTarget::Position1; break;
					case ExportTargetParameter: target = WriteToSliceTarget::Parameter0; break;
					default:
						// Primitive and allocation exports are NGG mechanics with no meaning once the
						// pair is lowered to an instanced vertex shader, so they are ignored rather
						// than rejected.
						continue;
				}
				for (uint32_t component = 0; component < 4u; component++) {
					if (((inst.exp.en >> component) & 1u) == 0) {
						continue;
					}
					const auto& operand = *operands[component];
					if (!IsVectorRegister(operand)) {
						continue;
					}
					if (const auto origin = tracker.RingOrigin(operand.reg)) {
						map.slots.push_back({target, component, true, *origin, 0});
					} else if (const auto constant = tracker.Constant(operand.reg)) {
						map.slots.push_back({target, component, false, 0, *constant});
					}
				}
				continue;
			}
			default: break;
		}

		TrackAddress(inst, map.ring_stride, tracker);
	}

	if (map.slots.empty()) {
		map.reject_reason = "no export component could be traced back to the ESGS ring";
		return map;
	}
	// A ring dword reaching two different export components means the geometry stage is not a plain
	// relabelling of its input, so eliding the ring would not preserve its behaviour.
	for (size_t a = 0; a < map.slots.size(); a++) {
		if (!map.slots[a].from_ring) {
			continue;
		}
		for (size_t b = a + 1; b < map.slots.size(); b++) {
			if (map.slots[b].from_ring && map.slots[a].ring_offset == map.slots[b].ring_offset) {
				map.reject_reason = fmt::format("ring offset {} feeds two export components",
				                                map.slots[a].ring_offset);
				return map;
			}
		}
	}

	map.lowerable = true;
	return map;
}

WriteToSlicePlan PlanWriteToSlice(const Program& es, const WriteToSliceMap& gs_map) {
	WriteToSlicePlan plan;

	if (!gs_map.lowerable) {
		plan.reject_reason = gs_map.reject_reason.empty() ? "geometry stage is not a passthrough"
		                                                  : gs_map.reject_reason;
		return plan;
	}

	const auto end = ProgramEnd(es);
	if (end == 0) {
		plan.reject_reason = "vertex shader has no terminator";
		return plan;
	}

	// Read the stride from the vertex stage too rather than trusting the geometry stage's. The two
	// describe the same ring, so a disagreement means these are not the pair they appear to be.
	plan.ring_stride = DeriveRingStride(es, end);
	if (plan.ring_stride == 0) {
		plan.reject_reason = "no ESGS ring stride could be derived from the vertex shader";
		return plan;
	}
	if (plan.ring_stride != gs_map.ring_stride) {
		plan.reject_reason = fmt::format("ring stride disagrees: vertex {} bytes, geometry {} bytes",
		                                 plan.ring_stride, gs_map.ring_stride);
		return plan;
	}

	const auto slot_for_offset = [&gs_map](uint32_t ring_offset) -> const WriteToSliceSlot* {
		for (const auto& slot: gs_map.slots) {
			if (slot.from_ring && slot.ring_offset == ring_offset) {
				return &slot;
			}
		}
		return nullptr;
	};

	Tracker tracker(plan.ring_stride);
	// A ring store whose data is not in a vector register cannot become an export, so it is a
	// rejection rather than something to skip - skipping it would leave that component unwritten.
	const auto record = [&](uint32_t pc, const Operand& data, uint32_t ring_offset) {
		const auto* slot = slot_for_offset(ring_offset);
		if (slot == nullptr) {
			// The geometry stage exports nothing from this dword, so eliding the ring elides the
			// store with it. Dead either way, and dropping it is the whole point of the lowering.
			return true;
		}
		if (!IsVectorRegister(data)) {
			plan.reject_reason =
			    fmt::format("ring offset {} is stored from a non-vector operand", ring_offset);
			return false;
		}
		plan.stores.push_back({pc, ring_offset, slot->target, slot->component});
		return true;
	};

	for (size_t i = 0; i < end; i++) {
		const auto& inst = es.instructions[i];

		if (inst.opcode == Opcode::DsWriteB32 || inst.opcode == Opcode::DsWrite2B32) {
			const auto base = IsVectorRegister(inst.src0) ? tracker.Address(inst.src0.reg)
			                                             : std::optional<AddressValue> {};
			if (base && base->scaled) {
				if (!record(inst.pc, inst.src1, base->constant + inst.offset)) {
					plan.stores.clear();
					return plan;
				}
				if (inst.opcode == Opcode::DsWrite2B32 &&
				    !record(inst.pc, inst.src2, base->constant + inst.secondary_offset)) {
					plan.stores.clear();
					return plan;
				}
			}
			continue;
		}

		TrackAddress(inst, plan.ring_stride, tracker);
	}

	for (const auto& slot: gs_map.slots) {
		if (!slot.from_ring) {
			plan.constants.push_back(slot);
			continue;
		}
		const auto stored = std::any_of(plan.stores.begin(), plan.stores.end(),
		                                [&slot](const WriteToSliceStore& store) {
			                                return store.ring_offset == slot.ring_offset;
		                                });
		if (!stored) {
			plan.reject_reason =
			    fmt::format("ring offset {} is exported but never stored", slot.ring_offset);
			plan.stores.clear();
			plan.constants.clear();
			return plan;
		}
	}

	plan.lowerable = true;
	return plan;
}

std::string WriteToSliceMapToString(const WriteToSliceMap& map) {
	if (!map.lowerable) {
		return fmt::format("; writetoslice: not lowerable ({})\n",
		                   map.reject_reason.empty() ? "unknown" : map.reject_reason);
	}
	std::string text = fmt::format("; writetoslice: ring stride {} bytes\n", map.ring_stride);
	for (const auto& slot: map.slots) {
		if (slot.from_ring) {
			text += fmt::format(";   ring+{:<3}   -> {}.{}\n", slot.ring_offset,
			                    TargetName(slot.target), "xyzw"[slot.component]);
		} else {
			text += fmt::format(";   const 0x{:08x} -> {}.{}\n", slot.constant,
			                    TargetName(slot.target), "xyzw"[slot.component]);
		}
	}
	return text;
}

std::string WriteToSlicePlanToString(const WriteToSlicePlan& plan) {
	if (!plan.lowerable) {
		return fmt::format("; writetoslice: pair not lowerable ({})\n",
		                   plan.reject_reason.empty() ? "unknown" : plan.reject_reason);
	}
	std::string text = fmt::format("; writetoslice: ring stride {} bytes, {} stores\n",
	                               plan.ring_stride, plan.stores.size());
	for (const auto& store: plan.stores) {
		text += fmt::format(";   pc 0x{:08x} ring+{:<3} -> {}.{}\n", store.pc, store.ring_offset,
		                    TargetName(store.target), "xyzw"[store.component]);
	}
	for (const auto& slot: plan.constants) {
		text += fmt::format(";   const 0x{:08x}      -> {}.{}\n", slot.constant,
		                    TargetName(slot.target), "xyzw"[slot.component]);
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
