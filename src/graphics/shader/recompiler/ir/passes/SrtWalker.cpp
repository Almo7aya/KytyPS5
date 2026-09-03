#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cinttypes>
#include <cstdio>
#include <fmt/format.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

std::string Diagnostic(const ResourcePlan& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
}

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t& result) {
	if (base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	result = base + magnitude;
	return true;
}

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto kind = values.memory_info[index].kind;
	return (op == ValueOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) ||
	       (op == ValueOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMin32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, Value active_mask = {})
	    : m_program(program), m_active_mask(active_mask.Resolve()) {}

	bool Run(Value value) { return Validate(value); }

private:
	bool Validate(Value value) {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return false;
			}
		}
		if (!m_visiting.insert(inst).second) {
			return false;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU8 ||
		    op == ValueOpcode::UndefU16 || op == ValueOpcode::UndefU32 ||
		    op == ValueOpcode::UndefU64 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			return finish(RuntimeValidator(m_program, inst->Arg(1)).Run(inst->Arg(0)));
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
		} else if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				return finish(false);
			}
		}
		if (op == ValueOpcode::GetBufferResource || op == ValueOpcode::GetImageResource ||
		    op == ValueOpcode::GetSamplerResource || op == ValueOpcode::GetAddressResource) {
			const size_t expected = op == ValueOpcode::GetBufferResource    ? 4u
			                        : op == ValueOpcode::GetImageResource   ? 8u
			                        : op == ValueOpcode::GetSamplerResource ? 4u
			                                                                : 2u;
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			if (!Validate(inst->Arg(index))) {
				return finish(false);
			}
		}
		return finish(true);
	}

	const ResourcePlan&             m_program;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
};

class PlanBuilder {
public:
	explicit PlanBuilder(Program& program): m_program(program) {}

	void Run() {
		m_program.srt_reads.clear();
		m_program.dynamic_reads.clear();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
					const auto flags = inst.Flags<MemoryFlags>();
					if (flags.index < m_program.memory_info.size()) {
						const auto kind       = m_program.memory_info[flags.index].kind;
						const bool crosswired = (op == ValueOpcode::LoadAddressU32 &&
						                         kind == ResourceKind::ScalarBuffer) ||
						                        (op == ValueOpcode::ReadConstBuffer &&
						                         kind == ResourceKind::ScalarAddress);
						if (crosswired) {
							Fail(flags.pc,
							     fmt::format("{} has incompatible scalar memory metadata",
							                 ValueOpcodeName(op)));
						}
					}
				}
				if (IsDescriptorHandle(inst.GetOpcode())) {
					for (size_t index = 0; index < inst.NumArgs(); index++) {
						Collect(inst.Arg(index), 0);
					}
				}
			}
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 && IsRawRead(m_program, inst) &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst))) {
					Collect(Value(&inst), inst.Flags<MemoryFlags>().pc);
				}
			}
		}
		PatchReads();
	}

private:
	struct Patch {
		Inst*    inst = nullptr;
		uint32_t slot = 0;
		bool     keep = false;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& message) const {
		const auto diagnostic = Diagnostic(m_program, pc, message);
		EXIT("shader SRT planning failed: %s", diagnostic.c_str());
		std::abort();
	}

	void Collect(Value value, uint32_t use_pc) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			Fail(use_pc, "invalid typed planning value");
		}
		const auto cycle = std::ranges::find(m_visiting, inst);
		if (cycle != m_visiting.end()) {
			const auto contains_phi = std::any_of(cycle, m_visiting.end(), [](const Inst* value) {
				return value->GetOpcode() == ValueOpcode::Phi;
			});
			if (contains_phi) {
				return;
			}
			Fail(use_pc, fmt::format("cyclic typed planning value {} without a phi",
			                         ValueOpcodeName(inst->GetOpcode())));
		}
		if (std::ranges::find(m_visited, inst) != m_visited.end()) {
			return;
		}
		m_visiting.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			Collect(inst->Arg(index), use_pc);
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_program, *inst)) {
			return;
		}
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) {
			if (std::ranges::find(m_program.dynamic_reads, value) ==
			    m_program.dynamic_reads.end()) {
				m_program.dynamic_reads.push_back(value);
			}
			return;
		}
		for (uint32_t slot = 0; slot < m_program.srt_reads.size(); slot++) {
			if (EquivalentValue(m_program, value, m_program.srt_reads[slot].value)) {
				m_patches.push_back({inst, slot, false});
				return;
			}
		}
		const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
		m_program.srt_reads.push_back({value, slot});
		m_patches.push_back({inst, slot, true});
	}

	void PatchReads() {
		for (const auto& patch: m_patches) {
			auto* block = patch.inst->Parent();
			auto& list  = block->Instructions();
			auto  where =
			    std::ranges::find_if(list, [&](const Inst& inst) { return &inst == patch.inst; });
			const auto resource =
			    Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			                                                {resource, Value(patch.slot)}));
			const auto uses = patch.inst->Uses();
			for (const auto& use: uses) {
				use.user->SetArg(use.operand, flat);
			}
			for (auto& info: m_program.block_info) {
				if (info.condition.Resolve() == Value(patch.inst)) {
					info.condition = flat;
				}
				if (info.indirect_target.Resolve() == Value(patch.inst)) {
					info.indirect_target = flat;
				}
			}
			if (patch.keep) {
				const auto memory = patch.inst->Flags<MemoryFlags>().index;
				if (memory < m_program.memory_info.size()) {
					m_program.memory_info[memory].planning_only = true;
				}
				block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(patch.inst)});
			}
		}
	}

	Program&           m_program;
	std::vector<Inst*> m_visiting;
	std::vector<Inst*> m_visited;
	std::vector<Patch> m_patches;
};

class Evaluator {
public:
	Evaluator(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, Evaluator* clean_evaluator = nullptr,
	          Value active_mask = {})
	    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
	      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()) {}

	bool Evaluate(Value value, uint32_t& result) {
		uint64_t wide = 0;
		if (!EvaluateWide(value, wide)) {
			return false;
		}
		result = static_cast<uint32_t>(wide);
		return true;
	}

private:
	static float Float32(uint64_t bits) {
		return std::bit_cast<float>(static_cast<uint32_t>(bits));
	}

	static uint64_t Float32Bits(float value) { return std::bit_cast<uint32_t>(value); }

	bool EvaluateWide(Value value, uint64_t& result) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: result = value.U1(); return true;
				case Type::U8: result = value.U8(); return true;
				case Type::U16: result = value.U16(); return true;
				case Type::U32: result = value.U32(); return true;
				case Type::U64: result = value.U64(); return true;
				case Type::F32: result = Float32Bits(value.F32Value()); return true;
				default: return false;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
		    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
			return EvaluateWide(inst->Arg(1), result);
		}
		const auto slot = inst->GetEvalSlot();
		if (slot < m_program.eval_slot_count) {
			// Fast path for extracted plans: flat memo and cycle state indexed by slot.
			if (m_slot_state.empty()) {
				m_slot_state.assign(m_program.eval_slot_count, SlotUnknown);
				m_slot_values.resize(m_program.eval_slot_count);
			}
			switch (m_slot_state[slot]) {
				case SlotDone:
					result = m_slot_values[slot];
					return true;
				case SlotVisiting: return false;
				default: break;
			}
			m_slot_state[slot] = SlotVisiting;
			uint64_t out       = 0;
			if (!EvaluateInst(*inst, out)) {
				m_slot_state[slot] = SlotUnknown;
				return false;
			}
			m_slot_state[slot]  = SlotDone;
			m_slot_values[slot] = out;
			result              = out;
			return true;
		}
		if (LookupCached(inst, result)) {
			return true;
		}
		if (std::ranges::find(m_visiting, inst) != m_visiting.end()) {
			return false;
		}
		m_visiting.push_back(inst);
		uint64_t out = 0;
		if (!EvaluateInst(*inst, out)) {
			return false;
		}
		m_visiting.pop_back();
		StoreCached(inst, out);
		result = out;
		return true;
	}

	static constexpr uint8_t SlotUnknown  = 0;
	static constexpr uint8_t SlotVisiting = 1;
	static constexpr uint8_t SlotDone     = 2;

	// An evaluator lives for a single draw and typically touches a few dozen values, so the cache
	// is a small flat array with a hash map only as an overflow for unusually deep SRT graphs.
	// Sizing a hash map to the whole shader's value count up front cost more than the walk itself.
	static constexpr size_t SmallCacheLimit = 96;

	bool LookupCached(const Inst* inst, uint64_t& result) const {
		for (const auto& [key, value]: m_small_cache) {
			if (key == inst) {
				result = value;
				return true;
			}
		}
		if (m_overflow_cache) {
			if (const auto found = m_overflow_cache->find(inst); found != m_overflow_cache->end()) {
				result = found->second;
				return true;
			}
		}
		return false;
	}

	void StoreCached(const Inst* inst, uint64_t value) {
		if (m_small_cache.size() < SmallCacheLimit) {
			if (m_small_cache.empty()) {
				m_small_cache.reserve(32);
			}
			m_small_cache.emplace_back(inst, value);
			return;
		}
		if (!m_overflow_cache) {
			m_overflow_cache = std::make_unique<std::unordered_map<const Inst*, uint64_t>>();
		}
		m_overflow_cache->emplace(inst, value);
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result) {
		return EvaluateWide(inst.Arg(index), result);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result) {
		const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
		return !value.IsEmpty() && EvaluateWide(value, result);
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return false;
		}
		const auto component = index.U32();
		if (component >= 2u) {
			return false;
		}
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			uint64_t packed = 0;
			if (!Arg(inst, 0, packed)) {
				return false;
			}
			result = static_cast<uint32_t>(packed >> (component * 32u));
			return true;
		}
		const auto* source = inst.Arg(0).ResolveInstruction();
		if (source == nullptr) {
			return false;
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return EvaluateWide(source->Arg(component), result);
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			uint64_t lhs = 0;
			uint64_t rhs = 0;
			if (!Arg(*source, 0, lhs) || !Arg(*source, 1, rhs)) {
				return false;
			}
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
			result =
			    component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
			return true;
		}
		return false;
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			return false;
		}
		const auto& mem    = m_program.memory_info[flags.index];
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return false;
		}
		uint64_t low    = 0;
		uint64_t high   = 0;
		uint64_t offset = 0;
		if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
			return false;
		}
		const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		uint64_t   address   = 0;
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			uint64_t records = 0;
			uint64_t word3   = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
				return false;
			}
			if (immediate < 0) {
				return false;
			}
			const auto byte_offset =
			    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
			const auto aligned = byte_offset & ~uint64_t {3};
			const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size = stride == 0u
			                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			if (aligned > size || size - aligned < sizeof(uint32_t)) {
				return false;
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto relative = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
				return false;
			}
		}
		uint32_t word = 0;
		if (m_runtime.read_memory != nullptr) {
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				return false;
			}
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	bool EvaluateInst(const Inst& inst, uint64_t& result) {
		uint64_t   a       = 0;
		uint64_t   b       = 0;
		uint64_t   c       = 0;
		const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
		const auto ternary = [&]() {
			return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base ||
				    reg - m_program.user_data_base >= m_runtime.user_data.size()) {
					return false;
				}
				result = m_runtime.user_data[reg - m_program.user_data_base];
				return true;
			}
			case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
			case ValueOpcode::Phi: return EvaluatePhi(inst, result);
			case ValueOpcode::ReadFirstLane: {
				Evaluator active(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
				                 inst.Arg(1));
				return active.EvaluateWide(inst.Arg(0), result);
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
			case ValueOpcode::CompositeConstructU64:
				if (!binary()) {
					return false;
				}
				result = static_cast<uint32_t>(a) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				return true;
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return false;
				}
				if (slot.U32() < m_clean_flat_slots.size() &&
				    m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr) {
					return m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
					                                       result);
				}
				return EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_program, inst)) {
					return EvaluateRawRead(inst, result);
				}
				break;
			case ValueOpcode::IAdd32:
				if (binary()) {
					result = static_cast<uint32_t>(a + b);
					return true;
				}
				return false;
			case ValueOpcode::IAdd64:
				if (binary()) {
					result = a + b;
					return true;
				}
				return false;
			case ValueOpcode::ISub32:
				if (binary()) {
					result = static_cast<uint32_t>(a - b);
					return true;
				}
				return false;
			case ValueOpcode::ISub64:
				if (binary()) {
					result = a - b;
					return true;
				}
				return false;
			case ValueOpcode::IMul32:
				if (binary()) {
					result = static_cast<uint32_t>(a * b);
					return true;
				}
				return false;
			case ValueOpcode::IMul64:
				if (binary()) {
					result = a * b;
					return true;
				}
				return false;
			case ValueOpcode::UMin32:
				if (binary()) {
					result = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
					return true;
				}
				return false;
			case ValueOpcode::ConvertF32U32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(static_cast<float>(static_cast<uint32_t>(a)));
					return true;
				}
				return false;
			case ValueOpcode::ConvertU32F32:
				if (Arg(inst, 0, a)) {
					const auto value = Float32(a);
					if (!std::isfinite(value) || value < 0.0f ||
					    static_cast<double>(value) > UINT32_MAX) {
						return false;
					}
					result = static_cast<uint32_t>(value);
					return true;
				}
				return false;
			case ValueOpcode::FPMul32:
				if (binary()) {
					result = Float32Bits(Float32(a) * Float32(b));
					return true;
				}
				return false;
			case ValueOpcode::FPTrunc32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(std::trunc(Float32(a)));
					return true;
				}
				return false;
			case ValueOpcode::FPIsNan32:
				if (Arg(inst, 0, a)) {
					result = std::isnan(Float32(a));
					return true;
				}
				return false;
			case ValueOpcode::FPOrdLessThanEqual32:
				if (binary()) {
					result = Float32(a) <= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::FPOrdGreaterThanEqual32:
				if (binary()) {
					result = Float32(a) >= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd32:
				if (binary()) {
					result = static_cast<uint32_t>(a & b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd64:
				if (binary()) {
					result = a & b;
					return true;
				}
				return false;
			case ValueOpcode::BitwiseOr32:
				if (binary()) {
					result = static_cast<uint32_t>(a | b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseXor32:
				if (binary()) {
					result = static_cast<uint32_t>(a ^ b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseNot32:
				if (Arg(inst, 0, a)) {
					result = ~static_cast<uint32_t>(a);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) << (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical64:
				if (binary()) {
					result = a << (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) >> (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical64:
				if (binary()) {
					result = a >> (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic32:
				if (binary()) {
					result = static_cast<uint32_t>(
					    std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >> (b & 31u));
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic64:
				if (binary()) {
					result = static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
					return true;
				}
				return false;
			case ValueOpcode::BitFieldUExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					const auto mask = width == 32u  ? UINT32_MAX
					                  : width == 0u ? 0u
					                                : (uint32_t {1} << width) - 1u;
					result = width == 0u ? 0u : (static_cast<uint32_t>(a) >> offset) & mask;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldSExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					if (width == 0u) {
						result = 0;
						return true;
					}
					const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
					auto       bits = (static_cast<uint32_t>(a) >> offset) & mask;
					if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
						bits |= ~mask;
					}
					result = bits;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldInsert: {
				uint64_t d = 0;
				if (!ternary() || !Arg(inst, 3, d)) {
					return false;
				}
				const auto offset = static_cast<uint32_t>(c);
				const auto width  = static_cast<uint32_t>(d);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					result = static_cast<uint32_t>(a);
					return true;
				}
				const auto mask =
				    width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
				result = (static_cast<uint32_t>(a) & ~mask) |
				         ((static_cast<uint32_t>(b) << offset) & mask);
				return true;
			}
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32:
				if (ternary()) {
					result = a != 0u ? b : c;
					return true;
				}
				return false;
			case ValueOpcode::IEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::INotEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) != static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::ULessThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::UGreaterThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) > static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::LogicalAnd:
				if (binary()) {
					result = (a != 0u) && (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalOr:
				if (binary()) {
					result = (a != 0u) || (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalXor:
				if (binary()) {
					result = (a != 0u) != (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalNot:
				if (Arg(inst, 0, a)) {
					result = a == 0u;
					return true;
				}
				return false;
			case ValueOpcode::UndefU1:
			case ValueOpcode::UndefU8:
			case ValueOpcode::UndefU16:
			case ValueOpcode::UndefU32:
			case ValueOpcode::UndefU64: return false;
			default: break;
		}
		return false;
	}

	const ResourcePlan&                       m_program;
	const SrtRuntime&                         m_runtime;
	std::span<const uint8_t>                  m_clean_flat_slots;
	Evaluator*                                m_clean_evaluator = nullptr;
	Value                                     m_active_mask;
	std::vector<std::pair<const Inst*, uint64_t>>              m_small_cache;
	std::unique_ptr<std::unordered_map<const Inst*, uint64_t>> m_overflow_cache;
	std::vector<const Inst*>                                   m_visiting;
	std::vector<uint64_t>                                      m_slot_values;
	std::vector<uint8_t>                                       m_slot_state;
};

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

} // namespace

// Flat evaluation program for a ResourcePlan. The interpreter above walks the value graph
// recursively with a memo per node; that costs tens of nanoseconds per visit and runs for every
// draw. The graph is static per plan, so it is compiled once into post-ordered ops over a slot
// array (Inst::GetEvalSlot for values, extra slots for immediates) and executed in one loop.
// Anything the compiler does not understand leaves the plan (or that descriptor source) on the
// interpreter. Results are cross-checked against the interpreter for the first evaluations of
// each plan; a mismatch disables the compiled path for that plan.
struct CompiledSrt {
	enum class Op : uint8_t {
		Const,
		UserData,
		ShaderBase,
		Alias,
		ExtractU64,
		CarryLow,
		CarryHigh,
		ConstructU64,
		RawReadAddress,
		RawReadBuffer,
		IAdd32,
		IAdd64,
		ISub32,
		ISub64,
		IMul32,
		IMul64,
		UMin32,
		ConvertF32U32,
		ConvertU32F32,
		FPMul32,
		FPTrunc32,
		FPIsNan32,
		FPOrdLessThanEqual32,
		FPOrdGreaterThanEqual32,
		BitwiseAnd32,
		BitwiseAnd64,
		BitwiseOr32,
		BitwiseXor32,
		BitwiseNot32,
		ShiftLeftLogical32,
		ShiftLeftLogical64,
		ShiftRightLogical32,
		ShiftRightLogical64,
		ShiftRightArithmetic32,
		ShiftRightArithmetic64,
		BitFieldUExtract,
		BitFieldSExtract,
		BitFieldInsert,
		Select,
		IEqual32,
		INotEqual32,
		ULessThan32,
		UGreaterThan32,
		LogicalAnd,
		LogicalOr,
		LogicalXor,
		LogicalNot,
	};

	struct Instr {
		Op       op  = Op::Const;
		uint32_t out = 0;
		uint32_t a   = 0;
		uint32_t b   = 0;
		uint32_t c   = 0;
		uint32_t d   = 0;
		uint64_t imm = 0;
	};

	static constexpr uint32_t Invalid = UINT32_MAX;

	std::vector<Instr>                   code;
	std::vector<std::array<uint32_t, 8>> source_slots;
	std::vector<uint8_t>                 source_compiled;
	std::vector<uint32_t>                read_slots;
	uint32_t                             slot_count = 0;
	bool                                 flat_ok    = false;
	bool                                 ok         = false;
};

namespace {

class SrtCompiler {
public:
	using Op = CompiledSrt::Op;
	static constexpr uint32_t Invalid = CompiledSrt::Invalid;

	explicit SrtCompiler(const ResourcePlan& program, CompiledSrt& out)
	    : m_program(program), m_out(out) {
		m_state.assign(program.eval_slot_count, 0);
		m_out.slot_count = program.eval_slot_count;
	}

	void Run() {
		if (m_program.eval_slot_count == 0) {
			m_out.ok = false;
			return;
		}
		m_out.source_slots.resize(m_program.descriptor_sources.size());
		m_out.source_compiled.assign(m_program.descriptor_sources.size(), 0);
		for (size_t index = 0; index < m_program.descriptor_sources.size(); index++) {
			const auto& source   = m_program.descriptor_sources[index];
			bool        compiled = source.dword_count <= 8;
			for (uint32_t dword = 0; compiled && dword < source.dword_count; dword++) {
				const auto slot = Compile(source.dwords[dword]);
				compiled        = slot != Invalid;
				m_out.source_slots[index][dword] = slot;
			}
			m_out.source_compiled[index] = compiled ? 1u : 0u;
		}
		m_out.read_slots.resize(m_program.srt_reads.size());
		m_out.flat_ok = true;
		for (size_t index = 0; index < m_program.srt_reads.size(); index++) {
			const auto slot          = Compile(m_program.srt_reads[index].value);
			m_out.read_slots[index] = slot;
			if (slot == Invalid) {
				m_out.flat_ok = false;
			}
		}
		m_out.ok = true;
	}

private:
	uint32_t NewSlot() { return m_out.slot_count++; }

	void Emit(Op op, uint32_t out, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0,
	          uint64_t imm = 0) {
		m_out.code.push_back({op, out, a, b, c, d, imm});
	}

	uint32_t Compile(Value value) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			uint64_t bits = 0;
			switch (value.GetType()) {
				case Type::U1: bits = value.U1(); break;
				case Type::U8: bits = value.U8(); break;
				case Type::U16: bits = value.U16(); break;
				case Type::U32: bits = value.U32(); break;
				case Type::U64: bits = value.U64(); break;
				case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); break;
				default: return Invalid;
			}
			const auto slot = NewSlot();
			Emit(Op::Const, slot, 0, 0, 0, 0, bits);
			return slot;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return Invalid;
		}
		const auto slot = inst->GetEvalSlot();
		if (slot >= m_program.eval_slot_count) {
			return Invalid;
		}
		if (m_state[slot] == 2) {
			return slot;
		}
		if (m_state[slot] == 1) {
			return Invalid;
		}
		m_state[slot] = 1;
		if (!CompileInst(*inst, slot)) {
			return Invalid;
		}
		m_state[slot] = 2;
		return slot;
	}

	bool Unary(const Inst& inst, Op op, uint32_t out) {
		const auto a = Compile(inst.Arg(0));
		if (a == Invalid) {
			return false;
		}
		Emit(op, out, a);
		return true;
	}

	bool Binary(const Inst& inst, Op op, uint32_t out) {
		const auto a = Compile(inst.Arg(0));
		const auto b = a == Invalid ? Invalid : Compile(inst.Arg(1));
		if (b == Invalid) {
			return false;
		}
		Emit(op, out, a, b);
		return true;
	}

	bool Ternary(const Inst& inst, Op op, uint32_t out) {
		const auto a = Compile(inst.Arg(0));
		const auto b = a == Invalid ? Invalid : Compile(inst.Arg(1));
		const auto c = b == Invalid ? Invalid : Compile(inst.Arg(2));
		if (c == Invalid) {
			return false;
		}
		Emit(op, out, a, b, c);
		return true;
	}

	bool CompileInst(const Inst& inst, uint32_t out) {
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				if (inst.NumArgs() != 1 || inst.Arg(0).GetType() != Type::ScalarReg) {
					return false;
				}
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base) {
					return false;
				}
				Emit(Op::UserData, out, 0, 0, 0, 0, reg - m_program.user_data_base);
				return true;
			}
			case ValueOpcode::GetShaderBase: Emit(Op::ShaderBase, out); return true;
			case ValueOpcode::Phi: {
				const auto invariant =
				    ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
				if (invariant.IsEmpty()) {
					return false;
				}
				const auto a = Compile(invariant);
				if (a == Invalid) {
					return false;
				}
				Emit(Op::Alias, out, a);
				return true;
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Unary(inst, Op::Alias, out);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: {
				const auto index = inst.Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
					return false;
				}
				const auto component = index.U32();
				if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
					const auto a = Compile(inst.Arg(0));
					if (a == Invalid) {
						return false;
					}
					Emit(Op::ExtractU64, out, a, 0, 0, 0, component);
					return true;
				}
				const auto* source = inst.Arg(0).ResolveInstruction();
				if (source == nullptr) {
					return false;
				}
				if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
					if (source->NumArgs() < 2) {
						return false;
					}
					const auto a = Compile(source->Arg(component));
					if (a == Invalid) {
						return false;
					}
					Emit(Op::Alias, out, a);
					return true;
				}
				if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
					if (source->NumArgs() < 2) {
						return false;
					}
					const auto a = Compile(source->Arg(0));
					const auto b = a == Invalid ? Invalid : Compile(source->Arg(1));
					if (b == Invalid) {
						return false;
					}
					Emit(component == 0u ? Op::CarryLow : Op::CarryHigh, out, a, b);
					return true;
				}
				return false;
			}
			case ValueOpcode::CompositeConstructU64: return Binary(inst, Op::ConstructU64, out);
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return false;
				}
				if (slot.U32() < m_program.clean_flat_slots.size() &&
				    m_program.clean_flat_slots[slot.U32()] != 0u) {
					return false;
				}
				const auto a = Compile(m_program.srt_reads[slot.U32()].value);
				if (a == Invalid) {
					return false;
				}
				Emit(Op::Alias, out, a);
				return true;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer: {
				if (!IsRawRead(m_program, inst) || inst.NumArgs() < 2) {
					return false;
				}
				const auto  flags  = inst.Flags<MemoryFlags>();
				const auto& mem    = m_program.memory_info[flags.index];
				const auto* handle = inst.Arg(0).ResolveInstruction();
				if (handle == nullptr || handle->NumArgs() < 2) {
					return false;
				}
				const auto low    = Compile(handle->Arg(0));
				const auto high   = low == Invalid ? Invalid : Compile(handle->Arg(1));
				const auto offset = high == Invalid ? Invalid : Compile(inst.Arg(1));
				if (offset == Invalid) {
					return false;
				}
				if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
					if (handle->NumArgs() != 4u ||
					    static_cast<int64_t>(static_cast<int32_t>(mem.offset)) < 0) {
						return false;
					}
					const auto records = Compile(handle->Arg(2));
					const auto word3   = records == Invalid ? Invalid : Compile(handle->Arg(3));
					if (word3 == Invalid) {
						return false;
					}
					Emit(Op::RawReadBuffer, out, low, high, offset, records, mem.offset);
					return true;
				}
				Emit(Op::RawReadAddress, out, low, high, offset, 0, mem.offset);
				return true;
			}
			case ValueOpcode::IAdd32: return Binary(inst, Op::IAdd32, out);
			case ValueOpcode::IAdd64: return Binary(inst, Op::IAdd64, out);
			case ValueOpcode::ISub32: return Binary(inst, Op::ISub32, out);
			case ValueOpcode::ISub64: return Binary(inst, Op::ISub64, out);
			case ValueOpcode::IMul32: return Binary(inst, Op::IMul32, out);
			case ValueOpcode::IMul64: return Binary(inst, Op::IMul64, out);
			case ValueOpcode::UMin32: return Binary(inst, Op::UMin32, out);
			case ValueOpcode::ConvertF32U32: return Unary(inst, Op::ConvertF32U32, out);
			case ValueOpcode::ConvertU32F32: return Unary(inst, Op::ConvertU32F32, out);
			case ValueOpcode::FPMul32: return Binary(inst, Op::FPMul32, out);
			case ValueOpcode::FPTrunc32: return Unary(inst, Op::FPTrunc32, out);
			case ValueOpcode::FPIsNan32: return Unary(inst, Op::FPIsNan32, out);
			case ValueOpcode::FPOrdLessThanEqual32:
				return Binary(inst, Op::FPOrdLessThanEqual32, out);
			case ValueOpcode::FPOrdGreaterThanEqual32:
				return Binary(inst, Op::FPOrdGreaterThanEqual32, out);
			case ValueOpcode::BitwiseAnd32: return Binary(inst, Op::BitwiseAnd32, out);
			case ValueOpcode::BitwiseAnd64: return Binary(inst, Op::BitwiseAnd64, out);
			case ValueOpcode::BitwiseOr32: return Binary(inst, Op::BitwiseOr32, out);
			case ValueOpcode::BitwiseXor32: return Binary(inst, Op::BitwiseXor32, out);
			case ValueOpcode::BitwiseNot32: return Unary(inst, Op::BitwiseNot32, out);
			case ValueOpcode::ShiftLeftLogical32: return Binary(inst, Op::ShiftLeftLogical32, out);
			case ValueOpcode::ShiftLeftLogical64: return Binary(inst, Op::ShiftLeftLogical64, out);
			case ValueOpcode::ShiftRightLogical32:
				return Binary(inst, Op::ShiftRightLogical32, out);
			case ValueOpcode::ShiftRightLogical64:
				return Binary(inst, Op::ShiftRightLogical64, out);
			case ValueOpcode::ShiftRightArithmetic32:
				return Binary(inst, Op::ShiftRightArithmetic32, out);
			case ValueOpcode::ShiftRightArithmetic64:
				return Binary(inst, Op::ShiftRightArithmetic64, out);
			case ValueOpcode::BitFieldUExtract: return Ternary(inst, Op::BitFieldUExtract, out);
			case ValueOpcode::BitFieldSExtract: return Ternary(inst, Op::BitFieldSExtract, out);
			case ValueOpcode::BitFieldInsert: {
				if (inst.NumArgs() < 4) {
					return false;
				}
				const auto a = Compile(inst.Arg(0));
				const auto b = a == Invalid ? Invalid : Compile(inst.Arg(1));
				const auto c = b == Invalid ? Invalid : Compile(inst.Arg(2));
				const auto d = c == Invalid ? Invalid : Compile(inst.Arg(3));
				if (d == Invalid) {
					return false;
				}
				Emit(Op::BitFieldInsert, out, a, b, c, d);
				return true;
			}
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32: return Ternary(inst, Op::Select, out);
			case ValueOpcode::IEqual32: return Binary(inst, Op::IEqual32, out);
			case ValueOpcode::INotEqual32: return Binary(inst, Op::INotEqual32, out);
			case ValueOpcode::ULessThan32: return Binary(inst, Op::ULessThan32, out);
			case ValueOpcode::UGreaterThan32: return Binary(inst, Op::UGreaterThan32, out);
			case ValueOpcode::LogicalAnd: return Binary(inst, Op::LogicalAnd, out);
			case ValueOpcode::LogicalOr: return Binary(inst, Op::LogicalOr, out);
			case ValueOpcode::LogicalXor: return Binary(inst, Op::LogicalXor, out);
			case ValueOpcode::LogicalNot: return Unary(inst, Op::LogicalNot, out);
			default: return false;
		}
	}

	const ResourcePlan&  m_program;
	CompiledSrt&         m_out;
	std::vector<uint8_t> m_state;
};

bool RunCompiled(const CompiledSrt& compiled, const SrtRuntime& runtime,
                 std::vector<uint64_t>& slots) {
	using Op = CompiledSrt::Op;
	slots.resize(compiled.slot_count);
	auto* s = slots.data();
	for (const auto& i: compiled.code) {
		uint64_t& out = s[i.out];
		switch (i.op) {
			case Op::Const: out = i.imm; break;
			case Op::UserData:
				if (i.imm >= runtime.user_data.size()) {
					return false;
				}
				out = runtime.user_data[i.imm];
				break;
			case Op::ShaderBase: out = runtime.shader_base; break;
			case Op::Alias: out = s[i.a]; break;
			case Op::ExtractU64: out = static_cast<uint32_t>(s[i.a] >> (i.imm * 32u)); break;
			case Op::CarryLow:
			case Op::CarryHigh: {
				const auto sum = static_cast<uint64_t>(static_cast<uint32_t>(s[i.a])) +
				                 static_cast<uint32_t>(s[i.b]);
				out = i.op == Op::CarryLow ? static_cast<uint32_t>(sum)
				                           : static_cast<uint32_t>(sum >> 32u);
				break;
			}
			case Op::ConstructU64:
				out = static_cast<uint32_t>(s[i.a]) |
				      (static_cast<uint64_t>(static_cast<uint32_t>(s[i.b])) << 32u);
				break;
			case Op::RawReadAddress:
			case Op::RawReadBuffer: {
				const auto low       = s[i.a];
				const auto high      = s[i.b];
				const auto offset    = s[i.c];
				const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
				const auto immediate = static_cast<int64_t>(static_cast<int32_t>(i.imm));
				uint64_t   address   = 0;
				if (i.op == Op::RawReadBuffer) {
					const auto records = s[i.d];
					const auto byte_offset =
					    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
					const auto aligned = byte_offset & ~uint64_t {3};
					const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
					const auto size    = stride == 0u
					                         ? static_cast<uint64_t>(static_cast<uint32_t>(records))
					                         : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
					if (aligned > size || size - aligned < sizeof(uint32_t)) {
						return false;
					}
					address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
				} else {
					const auto relative = (immediate & ~int64_t {3}) +
					                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
					if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
						return false;
					}
				}
				uint32_t word = 0;
				if (runtime.read_memory != nullptr) {
					if (!runtime.read_memory(runtime.userdata, address, &word)) {
						return false;
					}
				} else {
					std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
				}
				out = word;
				break;
			}
			case Op::IAdd32: out = static_cast<uint32_t>(s[i.a] + s[i.b]); break;
			case Op::IAdd64: out = s[i.a] + s[i.b]; break;
			case Op::ISub32: out = static_cast<uint32_t>(s[i.a] - s[i.b]); break;
			case Op::ISub64: out = s[i.a] - s[i.b]; break;
			case Op::IMul32: out = static_cast<uint32_t>(s[i.a] * s[i.b]); break;
			case Op::IMul64: out = s[i.a] * s[i.b]; break;
			case Op::UMin32:
				out = std::min(static_cast<uint32_t>(s[i.a]), static_cast<uint32_t>(s[i.b]));
				break;
			case Op::ConvertF32U32:
				out = std::bit_cast<uint32_t>(static_cast<float>(static_cast<uint32_t>(s[i.a])));
				break;
			case Op::ConvertU32F32: {
				const auto value = std::bit_cast<float>(static_cast<uint32_t>(s[i.a]));
				if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > UINT32_MAX) {
					return false;
				}
				out = static_cast<uint32_t>(value);
				break;
			}
			case Op::FPMul32:
				out = std::bit_cast<uint32_t>(std::bit_cast<float>(static_cast<uint32_t>(s[i.a])) *
				                              std::bit_cast<float>(static_cast<uint32_t>(s[i.b])));
				break;
			case Op::FPTrunc32:
				out = std::bit_cast<uint32_t>(
				    std::trunc(std::bit_cast<float>(static_cast<uint32_t>(s[i.a]))));
				break;
			case Op::FPIsNan32:
				out = std::isnan(std::bit_cast<float>(static_cast<uint32_t>(s[i.a]))) ? 1u : 0u;
				break;
			case Op::FPOrdLessThanEqual32:
				out = std::bit_cast<float>(static_cast<uint32_t>(s[i.a])) <=
				              std::bit_cast<float>(static_cast<uint32_t>(s[i.b]))
				          ? 1u
				          : 0u;
				break;
			case Op::FPOrdGreaterThanEqual32:
				out = std::bit_cast<float>(static_cast<uint32_t>(s[i.a])) >=
				              std::bit_cast<float>(static_cast<uint32_t>(s[i.b]))
				          ? 1u
				          : 0u;
				break;
			case Op::BitwiseAnd32: out = static_cast<uint32_t>(s[i.a] & s[i.b]); break;
			case Op::BitwiseAnd64: out = s[i.a] & s[i.b]; break;
			case Op::BitwiseOr32: out = static_cast<uint32_t>(s[i.a] | s[i.b]); break;
			case Op::BitwiseXor32: out = static_cast<uint32_t>(s[i.a] ^ s[i.b]); break;
			case Op::BitwiseNot32: out = ~static_cast<uint32_t>(s[i.a]); break;
			case Op::ShiftLeftLogical32:
				out = static_cast<uint32_t>(s[i.a]) << (s[i.b] & 31u);
				break;
			case Op::ShiftLeftLogical64: out = s[i.a] << (s[i.b] & 63u); break;
			case Op::ShiftRightLogical32:
				out = static_cast<uint32_t>(s[i.a]) >> (s[i.b] & 31u);
				break;
			case Op::ShiftRightLogical64: out = s[i.a] >> (s[i.b] & 63u); break;
			case Op::ShiftRightArithmetic32:
				out = static_cast<uint32_t>(std::bit_cast<int32_t>(static_cast<uint32_t>(s[i.a])) >>
				                            (s[i.b] & 31u));
				break;
			case Op::ShiftRightArithmetic64:
				out = static_cast<uint64_t>(std::bit_cast<int64_t>(s[i.a]) >> (s[i.b] & 63u));
				break;
			case Op::BitFieldUExtract: {
				const auto offset = static_cast<uint32_t>(s[i.b]);
				const auto width  = static_cast<uint32_t>(s[i.c]);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				const auto mask = width == 32u ? UINT32_MAX : width == 0u ? 0u : (uint32_t {1} << width) - 1u;
				out = width == 0u ? 0u : (static_cast<uint32_t>(s[i.a]) >> offset) & mask;
				break;
			}
			case Op::BitFieldSExtract: {
				const auto offset = static_cast<uint32_t>(s[i.b]);
				const auto width  = static_cast<uint32_t>(s[i.c]);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					out = 0;
					break;
				}
				const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
				auto       bits = (static_cast<uint32_t>(s[i.a]) >> offset) & mask;
				if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
					bits |= ~mask;
				}
				out = bits;
				break;
			}
			case Op::BitFieldInsert: {
				const auto offset = static_cast<uint32_t>(s[i.c]);
				const auto width  = static_cast<uint32_t>(s[i.d]);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					out = static_cast<uint32_t>(s[i.a]);
					break;
				}
				const auto mask = width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
				out = (static_cast<uint32_t>(s[i.a]) & ~mask) |
				      ((static_cast<uint32_t>(s[i.b]) << offset) & mask);
				break;
			}
			case Op::Select: out = s[i.a] != 0u ? s[i.b] : s[i.c]; break;
			case Op::IEqual32:
				out = static_cast<uint32_t>(s[i.a]) == static_cast<uint32_t>(s[i.b]) ? 1u : 0u;
				break;
			case Op::INotEqual32:
				out = static_cast<uint32_t>(s[i.a]) != static_cast<uint32_t>(s[i.b]) ? 1u : 0u;
				break;
			case Op::ULessThan32:
				out = static_cast<uint32_t>(s[i.a]) < static_cast<uint32_t>(s[i.b]) ? 1u : 0u;
				break;
			case Op::UGreaterThan32:
				out = static_cast<uint32_t>(s[i.a]) > static_cast<uint32_t>(s[i.b]) ? 1u : 0u;
				break;
			case Op::LogicalAnd: out = (s[i.a] != 0u) && (s[i.b] != 0u) ? 1u : 0u; break;
			case Op::LogicalOr: out = (s[i.a] != 0u) || (s[i.b] != 0u) ? 1u : 0u; break;
			case Op::LogicalXor: out = (s[i.a] != 0u) != (s[i.b] != 0u) ? 1u : 0u; break;
			case Op::LogicalNot: out = s[i.a] == 0u ? 1u : 0u; break;
		}
	}
	return true;
}

// Runs the compiled program for the requested sources. Returns false when the plan (or one of
// the requested sources) is not compiled or evaluation failed; the caller then interprets.
bool EvaluateCompiled(const ResourcePlan& program, std::span<const uint32_t> sources,
                      const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                      std::vector<uint32_t>& flat, bool evaluate_flat,
                      std::span<const uint8_t> clean_flat_slots) {
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; })) {
		return false;
	}
	if (!program.compiled_srt_attempted) {
		program.compiled_srt_attempted = true;
		auto compiled                  = std::make_shared<CompiledSrt>();
		SrtCompiler(program, *compiled).Run();
		if (compiled->ok) {
			program.compiled_srt = std::move(compiled);
		}
	}
	const auto* compiled = program.compiled_srt.get();
	if (compiled == nullptr || !compiled->ok || (evaluate_flat && !compiled->flat_ok)) {
		return false;
	}
	for (const auto source_index: sources) {
		if (source_index >= compiled->source_compiled.size() ||
		    compiled->source_compiled[source_index] == 0u) {
			return false;
		}
	}
	thread_local std::vector<uint64_t> slots;
	if (!RunCompiled(*compiled, runtime, slots)) {
		return false;
	}
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto& source = program.descriptor_sources[source_index];
		DescriptorValue value;
		value.dword_count = source.dword_count;
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			value.dwords[dword] =
			    static_cast<uint32_t>(slots[compiled->source_slots[source_index][dword]]);
		}
		evaluated.push_back(value);
	}
	if (evaluate_flat) {
		std::vector<uint32_t> flattened(program.srt_reads.size());
		for (size_t index = 0; index < program.srt_reads.size(); index++) {
			const auto offset = program.srt_reads[index].flat_offset;
			if (offset >= flattened.size()) {
				return false;
			}
			flattened[offset] = static_cast<uint32_t>(slots[compiled->read_slots[index]]);
		}
		flat = std::move(flattened);
	}
	results = std::move(evaluated);
	return true;
}

bool EvaluateRuntimeSourcesInterpreted(const ResourcePlan& program,
                                       std::span<const uint32_t> sources, const SrtRuntime& runtime,
                                       std::vector<DescriptorValue>& results,
                                       std::vector<uint32_t>& flat, bool evaluate_flat,
                                       std::span<const uint8_t> clean_flat_slots);

bool EvaluateRuntimeSourcesImpl(const ResourcePlan& program, std::span<const uint32_t> sources,
                                const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                                std::vector<uint32_t>& flat, bool evaluate_flat,
                                std::span<const uint8_t> clean_flat_slots) {
	if (!program.srt_plan_complete) {
		return false;
	}
	std::vector<DescriptorValue> fast_results;
	std::vector<uint32_t>        fast_flat;
	const bool fast_ok = EvaluateCompiled(program, sources, runtime, fast_results, fast_flat,
	                                      evaluate_flat, clean_flat_slots);
	if (fast_ok && program.compiled_srt_verify_left == 0) {
		results = std::move(fast_results);
		if (evaluate_flat) {
			flat = std::move(fast_flat);
		}
		return true;
	}
	std::vector<DescriptorValue> slow_results;
	std::vector<uint32_t>        slow_flat;
	const bool slow_ok = EvaluateRuntimeSourcesInterpreted(program, sources, runtime, slow_results,
	                                                       slow_flat, evaluate_flat,
	                                                       clean_flat_slots);
	if (fast_ok && program.compiled_srt_verify_left > 0) {
		program.compiled_srt_verify_left--;
		const bool same = slow_ok && slow_results == fast_results &&
		                  (!evaluate_flat || slow_flat == fast_flat);
		if (!same && program.compiled_srt) {
			std::printf("SRT: compiled evaluation mismatch for shader 0x%016" PRIx64
			            "; using the interpreter for this plan\n",
			            program.shader_hash);
			program.compiled_srt->ok = false;
		}
	}
	if (!slow_ok) {
		return false;
	}
	results = std::move(slow_results);
	if (evaluate_flat) {
		flat = std::move(slow_flat);
	}
	return true;
}

bool EvaluateRuntimeSourcesInterpreted(const ResourcePlan& program,
                                       std::span<const uint32_t> sources, const SrtRuntime& runtime,
                                       std::vector<DescriptorValue>& results,
                                       std::vector<uint32_t>& flat, bool evaluate_flat,
                                       std::span<const uint8_t> clean_flat_slots) {
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; }) &&
	    runtime.read_specialization_memory == nullptr) {
		return false;
	}
	SrtRuntime clean_runtime  = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	Evaluator                    clean_evaluator(program, clean_runtime);
	Evaluator                    evaluator(program, runtime, clean_flat_slots, &clean_evaluator);
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		for (uint32_t index = 0; index < source->dword_count; index++) {
			if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
				return false;
			}
		}
		evaluated.push_back(value);
	}
	std::vector<uint32_t> flattened;
	if (evaluate_flat) {
		flattened.resize(program.srt_reads.size());
		for (const auto& read: program.srt_reads) {
			const bool clean    = read.flat_offset < clean_flat_slots.size() &&
			                      clean_flat_slots[read.flat_offset] != 0u;
			auto&      selected = clean ? clean_evaluator : evaluator;
			if (read.flat_offset >= flattened.size() ||
			    !selected.Evaluate(read.value, flattened[read.flat_offset])) {
				return false;
			}
		}
	}
	results = std::move(evaluated);
	if (evaluate_flat) {
		flat = std::move(flattened);
	}
	return true;
}

} // namespace

bool ValidateRuntimeValue(const ResourcePlan& program, Value value) {
	return RuntimeValidator(program).Run(value);
}

void BuildSrtPlan(Program& program) {
	if (program.resource_tracking_complete) {
		EXIT("shader SRT planning failed: cannot rebuild SRT after resource tracking");
	}
	program.srt_plan_complete = false;
	PlanBuilder(program).Run();
	program.srt_plan_complete = true;
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result) {
	std::vector<DescriptorValue> results;
	if (!EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results) {
	std::vector<uint32_t> ignored;
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {});
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>&   flat,
                            std::span<const uint8_t> clean_flat_slots) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime,
             std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {});
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
