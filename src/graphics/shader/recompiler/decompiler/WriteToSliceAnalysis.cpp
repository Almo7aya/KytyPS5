#include "graphics/shader/recompiler/decompiler/WriteToSliceAnalysis.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::WriteToSlice {

namespace {

// A plausible ESGS vertex stride. The pattern packs a handful of dwords per vertex; anything outside
// this is not the shape we are recognizing.
constexpr uint32_t MinRingStride = 4;
constexpr uint32_t MaxRingStride = 256;

struct Provenance {
	SourceKind kind  = SourceKind::Unknown;
	uint32_t   value = 0;
};

// Tracks, per vector register, where its current contents came from. The GS reshuffles registers
// freely between reading the ring and exporting, so the link between the two can only be recovered
// by following the copies.
class Tracker {
public:
	// A ring base is `stride * vertexIndex + addend`. The GS stages its outputs in a second LDS
	// region before exporting, so the addend is what distinguishes the two.
	void SetRingBase(uint32_t reg, uint32_t addend) {
		Clear(reg);
		m_ring_bases[reg] = addend;
	}

	[[nodiscard]] bool GetRingBase(uint32_t reg, uint32_t& addend) const {
		const auto it = m_ring_bases.find(reg);
		if (it == m_ring_bases.end()) {
			return false;
		}
		addend = it->second;
		return true;
	}

	void Set(uint32_t reg, Provenance prov) {
		Clear(reg);
		if (prov.kind != SourceKind::Unknown) {
			m_prov[reg] = prov;
		}
	}

	[[nodiscard]] Provenance Get(uint32_t reg) const {
		const auto it = m_prov.find(reg);
		return it == m_prov.end() ? Provenance {} : it->second;
	}

	void Clear(uint32_t reg) {
		m_prov.erase(reg);
		m_ring_bases.erase(reg);
	}

	// Store-to-load forwarding across LDS, keyed by byte address within the vertex slot. This is
	// what lets the analysis see through the GS's own ring -> staging-area copy: a value written to
	// the staging area still remembers the ESGS offset it originally came from.
	//
	// The scan is control-flow-insensitive, and the GS performs that copy once per emitted vertex on
	// separate branch paths, so one key is written repeatedly. Disagreement between those writes is
	// the signal that the copy is *not* uniform across vertices, i.e. not a passthrough - so a
	// conflicting store poisons the key rather than overwriting it. A key therefore keeps a
	// provenance only if every store to it agreed, which is what makes the linear scan sound here.
	void StoreLds(uint32_t key, Provenance prov) {
		const auto it = m_lds.find(key);
		if (it == m_lds.end()) {
			m_lds.emplace(key, prov);
		} else if (it->second.kind != prov.kind || it->second.value != prov.value) {
			it->second = Provenance {};
		}
	}

	[[nodiscard]] Provenance LoadLds(uint32_t key, uint32_t stride) const {
		const auto it = m_lds.find(key);
		if (it != m_lds.end()) {
			return it->second;
		}
		// Never written by this shader, so it is an ES-produced ESGS dword - provided it lies
		// inside one vertex slot. Anything further out is some other LDS structure.
		return key < stride ? Provenance {SourceKind::RingOffset, key} : Provenance {};
	}

private:
	std::unordered_map<uint32_t, Provenance> m_prov;
	std::unordered_map<uint32_t, uint32_t>   m_ring_bases;
	std::unordered_map<uint32_t, Provenance> m_lds;
};

[[nodiscard]] bool IsVgpr(const Decoder::Operand& op) {
	return op.kind == Decoder::OperandKind::Vgpr;
}

// Returns the constant an operand carries, if it is one.
[[nodiscard]] bool ConstantBits(const Decoder::Operand& op, uint32_t& bits) {
	switch (op.kind) {
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::LiteralConstant: bits = op.value; return true;
		case Decoder::OperandKind::FloatInlineConstant:
			bits = std::bit_cast<uint32_t>(op.float_val);
			return true;
		default: return false;
	}
}

[[nodiscard]] const Decoder::Operand& SourceAt(const Decoder::Instruction& inst, uint32_t index) {
	switch (index) {
		case 0: return inst.src0;
		case 1: return inst.src1;
		case 2: return inst.src2;
		default: return inst.src3;
	}
}

// Whether an instruction defines its `dst` operand at all.
//
// The decoder fills `dst` from the instruction's vdst field unconditionally, and stores have no
// vdst - the bits read as register 0. So a store appears to define `v0`, and letting that clobber
// v0's provenance breaks the analysis outright: GTA III's GS carries ring[0] in v0 across the
// bookkeeping `ds_write`s of the NGG vertex-compaction pass.
//
// Only clearly-not-defining opcodes are listed. Anything unrecognized keeps clobbering, so a new
// store form costs a reject rather than a wrong map.
[[nodiscard]] bool DefinesDst(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::DsWriteByte:
		case Decoder::Opcode::DsWriteShort:
		case Decoder::Opcode::DsWriteB32:
		case Decoder::Opcode::DsWriteB64:
		case Decoder::Opcode::DsWriteB96:
		case Decoder::Opcode::DsWriteB128:
		case Decoder::Opcode::DsWrite2B32:
		case Decoder::Opcode::DsWrite2St64B32:
		case Decoder::Opcode::DsWrite2B64:
		case Decoder::Opcode::DsWrite2St64B64:
		case Decoder::Opcode::DsWriteAddtidB32:
		case Decoder::Opcode::BufferStoreFormatX:
		case Decoder::Opcode::BufferStoreFormatXy:
		case Decoder::Opcode::BufferStoreFormatXyz:
		case Decoder::Opcode::BufferStoreFormatXyzw:
		case Decoder::Opcode::BufferStoreByte:
		case Decoder::Opcode::BufferStoreShort:
		case Decoder::Opcode::BufferStoreDword:
		case Decoder::Opcode::BufferStoreDwordx2:
		case Decoder::Opcode::BufferStoreDwordx3:
		case Decoder::Opcode::BufferStoreDwordx4:
		case Decoder::Opcode::TBufferStoreFormatX:
		case Decoder::Opcode::TBufferStoreFormatXy:
		case Decoder::Opcode::TBufferStoreFormatXyz:
		case Decoder::Opcode::TBufferStoreFormatXyzw:
		case Decoder::Opcode::FlatStoreByte:
		case Decoder::Opcode::FlatStoreShort:
		case Decoder::Opcode::FlatStoreDword:
		case Decoder::Opcode::FlatStoreDwordx2:
		case Decoder::Opcode::FlatStoreDwordx3:
		case Decoder::Opcode::FlatStoreDwordx4:
		case Decoder::Opcode::ImageStore:
		case Decoder::Opcode::ImageStoreMip:
		case Decoder::Opcode::Exp: return false;
		default: return true;
	}
}

// How many consecutive vector registers an instruction defines. Memory loads write a run.
[[nodiscard]] uint32_t DefinedRegisterCount(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::DsReadB32:
		case Decoder::Opcode::DsRead2B32:
		case Decoder::Opcode::DsReadB64:
		case Decoder::Opcode::BufferLoadFormatX:
		case Decoder::Opcode::BufferLoadFormatXy:
		case Decoder::Opcode::BufferLoadFormatXyz:
		case Decoder::Opcode::BufferLoadFormatXyzw:
		case Decoder::Opcode::BufferLoadDword:
		case Decoder::Opcode::BufferLoadDwordx2:
		case Decoder::Opcode::BufferLoadDwordx3:
		case Decoder::Opcode::BufferLoadDwordx4: return std::max(1u, inst.data_dwords);
		default: return 1u;
	}
}

// Records the ring offsets an LDS read lands in. Returns false if the read shape is one we do not
// model, which forces a reject rather than a silently wrong map.
[[nodiscard]] bool RingReadOffsets(const Decoder::Instruction& inst,
                                   std::vector<uint32_t>&      offsets) {
	switch (inst.opcode) {
		case Decoder::Opcode::DsReadB32: offsets = {inst.offset}; return true;
		case Decoder::Opcode::DsRead2B32:
			offsets = {inst.offset, inst.secondary_offset};
			return true;
		case Decoder::Opcode::DsReadB64: offsets = {inst.offset, inst.offset + 4u}; return true;
		default: return false;
	}
}

// Finds the ESGS vertex stride, which the GS materializes as `v_mul_u32_u24 vX, <stride>, vY` when
// computing each input vertex's base address. All three input vertices must agree.
[[nodiscard]] bool FindStride(const Decoder::Program& gs, uint32_t& stride, std::string& reject) {
	bool found = false;
	for (const auto& inst: gs.instructions) {
		if (inst.opcode != Decoder::Opcode::VMulU32U24) {
			continue;
		}
		uint32_t bits = 0;
		if (!ConstantBits(inst.src0, bits) && !ConstantBits(inst.src1, bits)) {
			continue;
		}
		if (bits < MinRingStride || bits > MaxRingStride || (bits % 4u) != 0u) {
			continue;
		}
		if (found && bits != stride) {
			reject = fmt::format("conflicting ESGS strides {} and {}", stride, bits);
			return false;
		}
		stride = bits;
		found  = true;
	}
	if (!found) {
		reject = "no ESGS vertex stride found (expected v_mul_u32_u24 with a constant stride)";
		return false;
	}
	return true;
}

} // namespace

bool RingMap::HasTarget(uint32_t target) const {
	return std::any_of(components.begin(), components.end(),
	                   [target](const ExportComponent& c) { return c.target == target; });
}

RingMap AnalyzePassthroughGs(const Decoder::Program& gs) {
	RingMap result {};

	if (!FindStride(gs, result.stride, result.reject_reason)) {
		return result;
	}

	Tracker tracker;
	// Keyed by (target << 8) | component, so the three emitted vertices can be cross-checked against
	// each other: each must forward the same ring offset to the same slot.
	std::unordered_map<uint32_t, ExportComponent> slots;
	uint32_t                                      export_count = 0;

	for (const auto& inst: gs.instructions) {
		// Sources are read before destinations are clobbered - `ds_read_b32 v20, v20` relies on it.
		switch (inst.opcode) {
			case Decoder::Opcode::Exp: {
				// 0x09 null and 0x14 primitive-connectivity exports are NGG plumbing, not vertex
				// data: the host assembles primitives itself, so they carry nothing to forward.
				if (inst.exp.target == 0x09u || inst.exp.target == 0x14u) {
					continue;
				}
				export_count++;
				for (uint32_t c = 0; c < 4; c++) {
					if ((inst.exp.en & (1u << c)) == 0u) {
						continue;
					}
					const auto& src = SourceAt(inst, c);
					Provenance  prov {};
					uint32_t    bits = 0;
					if (IsVgpr(src)) {
						prov = tracker.Get(src.reg);
					} else if (ConstantBits(src, bits)) {
						prov = {SourceKind::Immediate, bits};
					}
					if (prov.kind == SourceKind::Unknown) {
						result.reject_reason = fmt::format(
						    "export target 0x{:02x} component {} at pc 0x{:x} is not a straight copy "
						    "of an ESGS ring dword",
						    inst.exp.target, c, inst.pc);
						return result;
					}

					const ExportComponent entry {inst.exp.target, c, prov.kind, prov.value};
					const uint32_t        key = (inst.exp.target << 8u) | c;
					const auto            it  = slots.find(key);
					if (it == slots.end()) {
						slots.emplace(key, entry);
					} else if (it->second.kind != entry.kind || it->second.value != entry.value) {
						// The emitted vertices disagree, so this is not a uniform passthrough.
						result.reject_reason = fmt::format(
						    "export target 0x{:02x} component {} is inconsistent across emitted "
						    "vertices",
						    inst.exp.target, c);
						return result;
					}
				}
				continue;
			}
			case Decoder::Opcode::VMulU32U24:
			case Decoder::Opcode::VMadU32U24: {
				// `stride * vertexIndex`, optionally offset into a second LDS region by a constant
				// third operand (v_mad_u32_u24 vX, <stride>, vIndex, <region base>).
				uint32_t bits = 0;
				if ((ConstantBits(inst.src0, bits) || ConstantBits(inst.src1, bits)) &&
				    bits == result.stride && IsVgpr(inst.dst)) {
					uint32_t addend = 0;
					if (inst.opcode == Decoder::Opcode::VMadU32U24 &&
					    !ConstantBits(inst.src2, addend)) {
						break; // a non-constant region base is not something we can follow
					}
					tracker.SetRingBase(inst.dst.reg, addend);
					continue;
				}
				break;
			}
			case Decoder::Opcode::DsWriteB32:
			case Decoder::Opcode::DsWrite2B32: {
				uint32_t addend = 0;
				if (IsVgpr(inst.src0) && tracker.GetRingBase(inst.src0.reg, addend)) {
					// src0 is the address; src1/src2 carry the data.
					const bool is_pair = inst.opcode == Decoder::Opcode::DsWrite2B32;
					const std::pair<const Decoder::Operand&, uint32_t> writes[] = {
					    {inst.src1, addend + inst.offset},
					    {inst.src2, addend + inst.secondary_offset},
					};
					for (uint32_t w = 0; w < (is_pair ? 2u : 1u); w++) {
						const auto& [data, key] = writes[w];
						Provenance  prov {};
						uint32_t    bits = 0;
						if (IsVgpr(data)) {
							prov = tracker.Get(data.reg);
						} else if (ConstantBits(data, bits)) {
							prov = {SourceKind::Immediate, bits};
						}
						tracker.StoreLds(key, prov);
					}
					continue;
				}
				break;
			}
			case Decoder::Opcode::VMovB32: {
				if (IsVgpr(inst.dst)) {
					uint32_t bits = 0;
					if (IsVgpr(inst.src0)) {
						tracker.Set(inst.dst.reg, tracker.Get(inst.src0.reg));
					} else if (ConstantBits(inst.src0, bits)) {
						tracker.Set(inst.dst.reg, {SourceKind::Immediate, bits});
					} else {
						tracker.Clear(inst.dst.reg);
					}
					continue;
				}
				break;
			}
			case Decoder::Opcode::DsReadB32:
			case Decoder::Opcode::DsRead2B32:
			case Decoder::Opcode::DsReadB64: {
				uint32_t addend = 0;
				if (IsVgpr(inst.src0) && tracker.GetRingBase(inst.src0.reg, addend) &&
				    IsVgpr(inst.dst)) {
					std::vector<uint32_t> offsets;
					if (!RingReadOffsets(inst, offsets)) {
						result.reject_reason =
						    fmt::format("unmodelled ESGS ring read at pc 0x{:x}", inst.pc);
						return result;
					}
					for (uint32_t i = 0; i < offsets.size(); i++) {
						// A hit resolves through the GS's own staging copy; a miss inside the
						// vertex slot is a dword the ES wrote.
						tracker.Set(inst.dst.reg + i,
						            tracker.LoadLds(addend + offsets[i], result.stride));
					}
					continue;
				}
				break;
			}
			default: break;
		}

		// Anything else destroys what we knew about the registers it defines.
		if (IsVgpr(inst.dst) && DefinesDst(inst)) {
			const auto count = DefinedRegisterCount(inst);
			for (uint32_t i = 0; i < count; i++) {
				tracker.Clear(inst.dst.reg + i);
			}
		}
		if (IsVgpr(inst.dst2)) {
			tracker.Clear(inst.dst2.reg);
		}
	}

	if (export_count == 0) {
		result.reject_reason = "geometry shader performs no exports";
		return result;
	}

	result.components.reserve(slots.size());
	for (const auto& [key, entry]: slots) {
		result.components.push_back(entry);
	}
	std::sort(result.components.begin(), result.components.end(),
	          [](const ExportComponent& a, const ExportComponent& b) {
		          return a.target != b.target ? a.target < b.target : a.component < b.component;
	          });

	// A vertex shader without a position export draws nothing, so refuse anything that lacks one.
	if (!result.HasTarget(0x0cu)) {
		result.reject_reason = "no POS0 export";
		return result;
	}

	result.matched = true;
	return result;
}

namespace {

// The offsets a DS store covers, and which source operand carries each. Mirrors LowerDsWrite and
// LowerDsWrite2 so the (pc, offset) pairs this produces name real IR instructions.
[[nodiscard]] bool RingWriteOffsets(const Decoder::Instruction& inst,
                                    std::vector<uint32_t>&      offsets) {
	switch (inst.opcode) {
		case Decoder::Opcode::DsWriteB32: offsets = {inst.offset}; return true;
		case Decoder::Opcode::DsWrite2B32:
			offsets = {inst.offset, inst.secondary_offset};
			return true;
		case Decoder::Opcode::DsWriteB64: offsets = {inst.offset, inst.offset + 4u}; return true;
		default: return false;
	}
}

// Every LDS opcode, so the plan can insist the ES touches LDS for nothing but the ESGS ring. If it
// did anything else there, dropping the ring would change what the shader computes.
[[nodiscard]] bool IsLdsAccess(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::DsAddU32:
		case Decoder::Opcode::DsAddRtnU32:
		case Decoder::Opcode::DsSubU32:
		case Decoder::Opcode::DsSubRtnU32:
		case Decoder::Opcode::DsMinI32:
		case Decoder::Opcode::DsMinRtnI32:
		case Decoder::Opcode::DsMaxI32:
		case Decoder::Opcode::DsMaxRtnI32:
		case Decoder::Opcode::DsMinU32:
		case Decoder::Opcode::DsMinRtnU32:
		case Decoder::Opcode::DsMaxU32:
		case Decoder::Opcode::DsMaxRtnU32:
		case Decoder::Opcode::DsAndB32:
		case Decoder::Opcode::DsAndRtnB32:
		case Decoder::Opcode::DsOrB32:
		case Decoder::Opcode::DsOrRtnB32:
		case Decoder::Opcode::DsXorB32:
		case Decoder::Opcode::DsXorRtnB32:
		case Decoder::Opcode::DsWrxchgRtnB32:
		case Decoder::Opcode::DsMinF32:
		case Decoder::Opcode::DsMaxF32:
		case Decoder::Opcode::DsConsume:
		case Decoder::Opcode::DsAppend:
		case Decoder::Opcode::DsReadSbyte:
		case Decoder::Opcode::DsReadUbyte:
		case Decoder::Opcode::DsReadSshort:
		case Decoder::Opcode::DsReadUshort:
		case Decoder::Opcode::DsRead2B32:
		case Decoder::Opcode::DsReadB32:
		case Decoder::Opcode::DsReadB64:
		case Decoder::Opcode::DsRead2B64:
		case Decoder::Opcode::DsRead2St64B64:
		case Decoder::Opcode::DsReadB96:
		case Decoder::Opcode::DsReadB128:
		case Decoder::Opcode::DsReadAddtidB32:
		case Decoder::Opcode::DsWriteByte:
		case Decoder::Opcode::DsWriteShort:
		case Decoder::Opcode::DsWrite2B32:
		case Decoder::Opcode::DsWrite2St64B32:
		case Decoder::Opcode::DsWrite2B64:
		case Decoder::Opcode::DsWrite2St64B64:
		case Decoder::Opcode::DsWriteB32:
		case Decoder::Opcode::DsWriteB64:
		case Decoder::Opcode::DsWriteB96:
		case Decoder::Opcode::DsWriteB128:
		case Decoder::Opcode::DsWriteAddtidB32:
		case Decoder::Opcode::DsSwizzleB32: return true;
		default: return false;
	}
}

[[nodiscard]] std::string ExportTargetName(uint32_t target) {
	return target == 0x0cu   ? "POS0"
	       : target == 0x0du ? "POS1"
	       : target >= 0x20u ? fmt::format("PARAM{}", target - 0x20u)
	                         : fmt::format("?{:#04x}", target);
}

} // namespace

EsPlan PlanEsExports(const Decoder::Program& es, const RingMap& map) {
	EsPlan result {};

	if (!map.matched) {
		result.reject_reason = "the geometry shader did not match";
		return result;
	}
	result.stride = map.stride;

	// Which ring offsets the GS actually forwards, and to where.
	std::unordered_map<uint32_t, const ExportComponent*> wanted;
	for (const auto& c: map.components) {
		if (c.kind == SourceKind::RingOffset) {
			wanted.emplace(c.value, &c);
		} else {
			result.constants.push_back(c);
		}
	}

	Tracker                     tracker;
	std::unordered_set<uint32_t> covered;

	for (const auto& inst: es.instructions) {
		// An export shader in a real ES+GS pair exports nothing - the GS owns every export. One here
		// means this is not the shape being recognized.
		if (inst.opcode == Decoder::Opcode::Exp) {
			result.reject_reason =
			    fmt::format("export shader exports directly at pc 0x{:x}", inst.pc);
			return result;
		}

		if (inst.opcode == Decoder::Opcode::VMulU32U24 ||
		    inst.opcode == Decoder::Opcode::VMadU32U24) {
			uint32_t bits = 0;
			if ((ConstantBits(inst.src0, bits) || ConstantBits(inst.src1, bits)) &&
			    bits == map.stride && IsVgpr(inst.dst)) {
				// The ESGS ring is addressed at exactly `stride * slot`. A constant addend would put
				// the store in some other LDS region, which this pattern does not have.
				uint32_t addend = 0;
				if (inst.opcode == Decoder::Opcode::VMadU32U24 &&
				    (!ConstantBits(inst.src2, addend) || addend != 0u)) {
					tracker.Clear(inst.dst.reg);
					continue;
				}
				tracker.SetRingBase(inst.dst.reg, 0);
				continue;
			}
		}

		if (IsLdsAccess(inst.opcode)) {
			std::vector<uint32_t> offsets;
			uint32_t              addend = 0;
			if (!IsVgpr(inst.src0) || !tracker.GetRingBase(inst.src0.reg, addend) ||
			    !RingWriteOffsets(inst, offsets)) {
				result.reject_reason = fmt::format(
				    "export shader uses LDS for something other than the ESGS ring at pc 0x{:x}",
				    inst.pc);
				return result;
			}
			for (uint32_t i = 0; i < offsets.size(); i++) {
				const auto offset = offsets[i];
				const auto it     = wanted.find(offset);
				if (it == wanted.end()) {
					// The GS never reads this dword back, so nothing consumes it. Dropping it is what
					// eliding the ring means; it is not evidence against the pattern.
					continue;
				}
				if (!covered.insert(offset).second) {
					result.reject_reason =
					    fmt::format("ESGS ring offset {} is written more than once", offset);
					return result;
				}
				result.stores.push_back({inst.pc, offset, it->second->target, it->second->component});
			}
			continue;
		}

		if (IsVgpr(inst.dst) && DefinesDst(inst)) {
			const auto count = DefinedRegisterCount(inst);
			for (uint32_t i = 0; i < count; i++) {
				tracker.Clear(inst.dst.reg + i);
			}
		}
		if (IsVgpr(inst.dst2)) {
			tracker.Clear(inst.dst2.reg);
		}
	}

	for (const auto& [offset, component]: wanted) {
		if (!covered.contains(offset)) {
			result.reject_reason = fmt::format(
			    "the geometry shader forwards ESGS ring offset {} but the export shader never writes "
			    "it",
			    offset);
			return result;
		}
	}

	std::sort(result.stores.begin(), result.stores.end(),
	          [](const RingStore& a, const RingStore& b) {
		          return a.pc != b.pc ? a.pc < b.pc : a.offset < b.offset;
	          });

	result.matched = true;
	return result;
}

std::string RingMapToString(const RingMap& map) {
	if (!map.matched) {
		return fmt::format("WriteToSlice: no match ({})\n",
		                   map.reject_reason.empty() ? "unknown reason" : map.reject_reason);
	}

	std::string out = fmt::format("WriteToSlice: matched, ESGS stride {} bytes\n", map.stride);
	for (const auto& c: map.components) {
		const auto target_name = ExportTargetName(c.target);
		const char swizzle     = "xyzw"[c.component & 3u];
		if (c.kind == SourceKind::RingOffset) {
			out += fmt::format("  ring[{:2}] -> {}.{}\n", c.value, target_name, swizzle);
		} else {
			out += fmt::format("  const 0x{:08x} -> {}.{}\n", c.value, target_name, swizzle);
		}
	}
	return out;
}

std::string EsPlanToString(const EsPlan& plan) {
	if (!plan.matched) {
		return fmt::format("WriteToSlice ES: no match ({})\n",
		                   plan.reject_reason.empty() ? "unknown reason" : plan.reject_reason);
	}

	std::string out = fmt::format("WriteToSlice ES: matched, {} ring stores retargeted\n",
	                              plan.stores.size());
	for (const auto& s: plan.stores) {
		out += fmt::format("  pc 0x{:04x} ring[{:2}] -> {}.{}\n", s.pc, s.offset,
		                   ExportTargetName(s.target), "xyzw"[s.component & 3u]);
	}
	for (const auto& c: plan.constants) {
		out += fmt::format("  const 0x{:08x} -> {}.{}\n", c.value, ExportTargetName(c.target),
		                   "xyzw"[c.component & 3u]);
	}
	return out;
}

} // namespace Libs::Graphics::ShaderRecompiler::WriteToSlice
