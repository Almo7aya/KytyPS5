#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"

#include <fmt/format.h>

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
			case Opcode::VMulU32U24:
			case Opcode::VMadU32U24: {
				const auto scale = InlineConstant(inst.src0);
				if (IsVectorRegister(inst.dst) && scale && *scale == map.ring_stride) {
					const auto bias = inst.opcode == Opcode::VMadU32U24
					                      ? InlineConstant(inst.src2).value_or(0)
					                      : 0;
					tracker.SetAddress(inst.dst.reg, {bias, true});
					continue;
				}
				break;
			}
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
			case Opcode::VAddNcU32: {
				// `base + constant` keeps addressing the same relocation region.
				if (IsVectorRegister(inst.dst) && IsVectorRegister(inst.src1)) {
					if (const auto base = tracker.Address(inst.src1.reg)) {
						if (const auto bias = InlineConstant(inst.src0)) {
							tracker.SetAddress(inst.dst.reg,
							                   {base->constant + *bias, base->scaled});
							continue;
						}
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

		if (DefinesDestination(inst)) {
			if (IsVectorRegister(inst.dst)) {
				tracker.Kill(inst.dst.reg);
			}
			if (IsVectorRegister(inst.dst2)) {
				tracker.Kill(inst.dst2.reg);
			}
		}
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

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
