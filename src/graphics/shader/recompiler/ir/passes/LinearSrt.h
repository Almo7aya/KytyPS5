#ifndef KYTY_LINEAR_SRT_H
#define KYTY_LINEAR_SRT_H

#include "common/abi.h"
#if defined(__x86_64__) || defined(_M_X64)
#ifndef XBYAK_NO_EXCEPTION
#define XBYAK_NO_EXCEPTION
#endif
#include <xbyak/xbyak.h>
#endif
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <set>

namespace Libs::Graphics::ShaderRecompiler::IR {

// One immutable program for a complete descriptor/flat transaction. Integer
// expressions become native code, shared reads execute once in their original
// order, and no evaluator callback or IR lookup occurs during execution.
struct LinearSrtPlan {
	enum class Kind { Immediate, User, Base, Unary, Binary, Select, Read };
	struct Node {
		Kind                    kind      = Kind::Immediate;
		ValueOpcode             op        = ValueOpcode::Void;
		uint64_t                immediate = 0;
		std::array<uint32_t, 5> args {};
		bool                    clean = false;
	};
	std::vector<Node>                                 nodes;
	std::vector<uint32_t>                             descriptor_words, flat_words, sources;
	std::vector<uint8_t>                              clean_slots, descriptor_sizes;
	std::vector<uint8_t>                              active_sources;
	std::vector<std::shared_ptr<const LinearSrtPlan>> control_variants;
	uint32_t                                          active_count = 0;
	// The generated register convention is explicit on every host, including
	// Windows. C++ helpers bridge to the host ABI of reader callbacks.
	using Function    = KYTY_SYSV_ABI bool (*)(const SrtRuntime*, uint64_t*);
	Function function = nullptr;
#if defined(__x86_64__) || defined(_M_X64)
	std::unique_ptr<Xbyak::CodeGenerator> code;
#endif

	// Keep the exact scalar-read bounds/rounding of Evaluator::EvaluateRawRead.
	static bool KYTY_SYSV_ABI Read(const SrtRuntime* runtime, const Node* node,
	                               const uint64_t* values, uint64_t* output) {
		const auto     low = values[node->args[0]], high = values[node->args[1]];
		const auto     offset = values[node->args[2]];
		const uint64_t base   = ((high << 32) | static_cast<uint32_t>(low)) & 0x0000ffffffffffffull;
		const auto     immediate = static_cast<int64_t>(static_cast<int32_t>(node->immediate));
		uint64_t       address   = 0;
		if (node->op == ValueOpcode::ReadConstBuffer) {
			const auto records = static_cast<uint32_t>(values[node->args[3]]);
			if (immediate < 0) return false;
			const auto     byte_offset = uint64_t(immediate) + static_cast<uint32_t>(offset);
			const auto     aligned     = byte_offset & ~uint64_t {3};
			const auto     stride      = (static_cast<uint32_t>(high) >> 16) & 0x3fff;
			const uint64_t size        = stride == 0 ? records : uint64_t(stride) * records;
			if (aligned > size || size - aligned < 4) return false;
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto relative =
			    (immediate & ~int64_t {3}) + int64_t(static_cast<uint32_t>(offset) & ~3u);
			const auto aligned_base = base & ~uint64_t {3};
			if (relative < 0) {
				if (uint64_t(-relative) > aligned_base) return false;
				address = aligned_base - uint64_t(-relative);
			} else {
				if (uint64_t(relative) > 0x0000ffffffffffffull - aligned_base) return false;
				address = aligned_base + uint64_t(relative);
			}
		}
		uint32_t   value = 0;
		const auto reader =
		    node->clean ? runtime->read_specialization_memory : runtime->read_memory;
		if (reader) {
			if (!reader(runtime->userdata, address, &value)) return false;
		} else {
			if (node->clean) return false;
			std::memcpy(&value, reinterpret_cast<const void*>(address), 4);
		}
		*output = value;
		return true;
	}
};

#if defined(__x86_64__) || defined(_M_X64)
class LinearSrtCompiler {
	const ResourcePlan&                              source;
	LinearSrtPlan&                                   result;
	std::span<const uint8_t>                         active_sources;
	std::map<std::pair<const Inst*, bool>, uint32_t> known;
	std::set<std::pair<const Inst*, bool>>           visiting;
	static constexpr uint32_t                        Invalid = UINT32_MAX;
	using Kind                                               = LinearSrtPlan::Kind;
	using Node                                               = LinearSrtPlan::Node;

	static bool Binary(ValueOpcode op) {
		switch (op) {
			case ValueOpcode::IAdd32:
			case ValueOpcode::IAdd64:
			case ValueOpcode::ISub32:
			case ValueOpcode::ISub64:
			case ValueOpcode::IMul32:
			case ValueOpcode::IMul64:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseAnd64:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftLeftLogical64:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightLogical64:
			case ValueOpcode::ShiftRightArithmetic32:
			case ValueOpcode::ShiftRightArithmetic64:
			case ValueOpcode::CompositeConstructU64:
			case ValueOpcode::IEqual32:
			case ValueOpcode::INotEqual32:
			case ValueOpcode::ULessThan32:
			case ValueOpcode::UGreaterThan32:
			case ValueOpcode::UGreaterThanEqual32:
			case ValueOpcode::UMin32:
			case ValueOpcode::LogicalAnd:
			case ValueOpcode::LogicalOr:
			case ValueOpcode::LogicalXor: return true;
			default: return false;
		}
	}
	uint32_t Add(Value value, bool clean, unsigned depth = 0) {
		if (depth > 64 || result.nodes.size() >= 8192) return Invalid;
		Node node;
		node.clean       = clean;
		const Inst* inst = nullptr;
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: node.immediate = value.U1(); break;
				case Type::U8: node.immediate = value.U8(); break;
				case Type::U16: node.immediate = value.U16(); break;
				case Type::U32: node.immediate = value.U32(); break;
				case Type::U64: node.immediate = value.U64(); break;
				case Type::F32: node.immediate = std::bit_cast<uint32_t>(value.F32Value()); break;
				default: return Invalid;
			}
		} else {
			inst = value.TryInstruction();
			if (!inst) return Invalid;
			const auto key = std::pair {inst, clean};
			if (auto found = known.find(key); found != known.end()) return found->second;
			if (!visiting.insert(key).second) return Invalid;
			struct Pop {
				decltype(visiting)& set;
				decltype(key)       key_value;
				~Pop() { set.erase(key_value); }
			} pop {visiting, key};
			node.op          = inst->GetOpcode();
			const auto arg   = [&](size_t i) { return Add(inst->Arg(i), clean, depth + 1); };
			const auto alias = [&](uint32_t index) {
				if (index != Invalid) known.emplace(key, index);
				return index;
			};
			switch (node.op) {
				case ValueOpcode::Identity:
				case ValueOpcode::BitCastU32F32:
				case ValueOpcode::BitCastF32U32: return alias(arg(0));
				case ValueOpcode::GetUserData: {
					const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
					if (reg < source.user_data_base) return Invalid;
					node.kind      = Kind::User;
					node.immediate = reg - source.user_data_base;
					break;
				}
				case ValueOpcode::GetShaderBase: node.kind = Kind::Base; break;
				case ValueOpcode::ReadConst: {
					const auto slot = inst->Arg(1).Resolve();
					if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
					    slot.U32() >= source.srt_reads.size())
						return Invalid;
					const bool use_clean = clean || (slot.U32() < result.clean_slots.size() &&
					                                 result.clean_slots[slot.U32()]);
					return alias(Add(source.srt_reads[slot.U32()].value, use_clean, depth + 1));
				}
				case ValueOpcode::CompositeExtractU32x2: {
					const auto  component = inst->Arg(1).Resolve();
					const auto* packed    = inst->Arg(0).ResolveInstruction();
					if (!packed || !component.IsImmediate() || component.GetType() != Type::U32 ||
					    component.U32() >= 2 ||
					    packed->GetOpcode() != ValueOpcode::CompositeConstructU32x2)
						return Invalid;
					return alias(Add(packed->Arg(component.U32()), clean, depth + 1));
				}
				case ValueOpcode::CompositeExtractU64: {
					const auto component = inst->Arg(1).Resolve();
					if (!component.IsImmediate() || component.GetType() != Type::U32 ||
					    component.U32() >= 2)
						return Invalid;
					node.kind      = Kind::Unary;
					node.immediate = component.U32();
					node.args[0]   = arg(0);
					break;
				}
				case ValueOpcode::BitwiseNot32:
				case ValueOpcode::LogicalNot:
					node.kind    = Kind::Unary;
					node.args[0] = arg(0);
					break;
				case ValueOpcode::SelectU32:
				case ValueOpcode::SelectU1:
				case ValueOpcode::SelectF32:
					// The original evaluator eagerly evaluates all three operands.
					node.kind = Kind::Select;
					for (int i = 0; i < 3; ++i)
						node.args[i] = arg(i);
					break;
				case ValueOpcode::LoadAddressU32:
				case ValueOpcode::ReadConstBuffer: {
					const auto  memory = inst->Flags<MemoryFlags>().index;
					const auto* handle = inst->Arg(0).ResolveInstruction();
					const bool  buffer = node.op == ValueOpcode::ReadConstBuffer;
					if (memory >= source.memory_info.size() || !handle ||
					    handle->NumArgs() != (buffer ? 4u : 2u) ||
					    source.memory_info[memory].kind !=
					        (buffer ? ResourceKind::ScalarBuffer : ResourceKind::ScalarAddress))
						return Invalid;
					node.kind      = Kind::Read;
					node.immediate = source.memory_info[memory].offset;
					node.args[0]   = Add(handle->Arg(0), clean, depth + 1);
					node.args[1]   = Add(handle->Arg(1), clean, depth + 1);
					node.args[2]   = arg(1);
					if (buffer)
						for (int i = 2; i < 4; ++i)
							node.args[i + 1] = Add(handle->Arg(i), clean, depth + 1);
					break;
				}
				default:
					if (!Binary(node.op)) return Invalid;
					node.kind    = Kind::Binary;
					node.args[0] = arg(0);
					node.args[1] = arg(1);
					break;
			}
			if (std::ranges::find(node.args, Invalid) != node.args.end()) return Invalid;
		}
		const uint32_t index = result.nodes.size();
		result.nodes.push_back(node);
		if (inst) known.emplace(std::pair {inst, clean}, index);
		return index;
	}

public:
	LinearSrtCompiler(const ResourcePlan& input, LinearSrtPlan& output,
	                  std::span<const uint8_t> active = {})
	    : source(input), result(output), active_sources(active) {}
	bool Build() {
		if (!source.srt_plan_complete || (!source.control_flow.empty() && active_sources.empty()))
			return false;
		if (!active_sources.empty() && active_sources.size() != source.descriptor_sources.size())
			return false;
		result.active_sources.assign(active_sources.begin(), active_sources.end());
		result.sources      = source.materialization_sources;
		result.clean_slots  = source.clean_flat_slots;
		result.active_count = source.descriptor_sources.size();
		for (const auto source_index: result.sources) {
			if (source_index >= source.descriptor_sources.size()) return false;
			const auto& descriptor = source.descriptor_sources[source_index];
			if (descriptor.dword_count > descriptor.dwords.size()) return false;
			result.descriptor_sizes.push_back(descriptor.dword_count);
			for (uint32_t i = 0; i < descriptor.dword_count; ++i) {
				if (!active_sources.empty() && !active_sources[source_index]) {
					result.descriptor_words.push_back(Invalid);
					continue;
				}
				const auto index = Add(descriptor.dwords[i], false);
				if (index == Invalid) return false;
				result.descriptor_words.push_back(index);
			}
		}
		result.flat_words.assign(source.srt_reads.size(), Invalid);
		for (const auto& read: source.srt_reads) {
			if (read.flat_offset >= result.flat_words.size()) return false;
			const auto index = Add(read.value, read.flat_offset < result.clean_slots.size() &&
			                                       result.clean_slots[read.flat_offset]);
			if (index == Invalid) return false;
			result.flat_words[read.flat_offset] = index;
		}
		if (result.nodes.empty() && active_sources.empty()) return false;
		Xbyak::ClearError();
		result.code = std::make_unique<Xbyak::CodeGenerator>(4096, Xbyak::AutoGrow);
		if (Xbyak::GetError() != 0 || !result.code->getCode()) {
			Xbyak::ClearError();
			return false;
		}
		auto& c = *result.code;
		using namespace Xbyak::util;
		Xbyak::Label fail, done;
		c.push(r12);
		c.push(r13);
		c.sub(rsp, 8);
		c.mov(r12, rdi);
		c.mov(r13, rsi);
		// SrtRuntime::user_data is a standard-library span. Its physical layout
		// stays in a C++ accessor instead of embedding an ABI assumption here.
		for (uint32_t i = 0; i < result.nodes.size(); ++i) {
			const auto& n = result.nodes[i];
			switch (n.kind) {
				case Kind::Immediate: c.mov(rax, n.immediate); break;
				case Kind::User:
					c.mov(rdi, r12);
					c.mov(rsi, n.immediate);
					c.lea(rdx, c.ptr[r13 + i * 8]);
					c.mov(rax, reinterpret_cast<uint64_t>(&ReadUser));
					c.call(rax);
					c.test(al, al);
					c.jz(fail, Xbyak::CodeGenerator::T_NEAR);
					continue;
				case Kind::Base:
					c.mov(rax, c.qword[r12 + offsetof(SrtRuntime, shader_base)]);
					break;
				case Kind::Read:
					c.mov(rdi, r12);
					c.mov(rsi, reinterpret_cast<uint64_t>(&n));
					c.mov(rdx, r13);
					c.lea(rcx, c.ptr[r13 + i * 8]);
					c.mov(rax, reinterpret_cast<uint64_t>(&LinearSrtPlan::Read));
					c.call(rax);
					c.test(al, al);
					c.jz(fail, Xbyak::CodeGenerator::T_NEAR);
					continue;
				case Kind::Unary:
					c.mov(rax, c.qword[r13 + n.args[0] * 8]);
					if (n.op == ValueOpcode::CompositeExtractU64) {
						if (n.immediate) c.shr(rax, 32);
						c.mov(eax, eax);
					} else if (n.op == ValueOpcode::BitwiseNot32)
						c.not_(eax);
					else {
						c.test(rax, rax);
						c.setz(al);
						c.movzx(eax, al);
					}
					break;
				case Kind::Select:
					c.mov(rax, c.qword[r13 + n.args[1] * 8]);
					c.mov(rcx, c.qword[r13 + n.args[2] * 8]);
					c.cmp(c.qword[r13 + n.args[0] * 8], 0);
					c.cmovz(rax, rcx);
					break;
				case Kind::Binary:
					c.mov(rax, c.qword[r13 + n.args[0] * 8]);
					c.mov(rcx, c.qword[r13 + n.args[1] * 8]);
					switch (n.op) {
						case ValueOpcode::IAdd32: c.add(eax, ecx); break;
						case ValueOpcode::IAdd64: c.add(rax, rcx); break;
						case ValueOpcode::ISub32: c.sub(eax, ecx); break;
						case ValueOpcode::ISub64: c.sub(rax, rcx); break;
						case ValueOpcode::IMul32: c.imul(eax, ecx); break;
						case ValueOpcode::IMul64: c.imul(rax, rcx); break;
						case ValueOpcode::BitwiseAnd32: c.and_(eax, ecx); break;
						case ValueOpcode::BitwiseAnd64: c.and_(rax, rcx); break;
						case ValueOpcode::BitwiseOr32: c.or_(eax, ecx); break;
						case ValueOpcode::BitwiseXor32: c.xor_(eax, ecx); break;
						case ValueOpcode::ShiftLeftLogical32: c.shl(eax, cl); break;
						case ValueOpcode::ShiftLeftLogical64: c.shl(rax, cl); break;
						case ValueOpcode::ShiftRightLogical32: c.shr(eax, cl); break;
						case ValueOpcode::ShiftRightLogical64: c.shr(rax, cl); break;
						case ValueOpcode::ShiftRightArithmetic32: c.sar(eax, cl); break;
						case ValueOpcode::ShiftRightArithmetic64: c.sar(rax, cl); break;
						case ValueOpcode::CompositeConstructU64:
							c.mov(eax, eax);
							c.shl(rcx, 32);
							c.or_(rax, rcx);
							break;
						case ValueOpcode::IEqual32:
							c.cmp(eax, ecx);
							c.sete(al);
							c.movzx(eax, al);
							break;
						case ValueOpcode::INotEqual32:
							c.cmp(eax, ecx);
							c.setne(al);
							c.movzx(eax, al);
							break;
						case ValueOpcode::ULessThan32:
							c.cmp(eax, ecx);
							c.setb(al);
							c.movzx(eax, al);
							break;
						case ValueOpcode::UGreaterThan32:
							c.cmp(eax, ecx);
							c.seta(al);
							c.movzx(eax, al);
							break;
						case ValueOpcode::UGreaterThanEqual32:
							c.cmp(eax, ecx);
							c.setae(al);
							c.movzx(eax, al);
							break;
						case ValueOpcode::UMin32:
							c.cmp(eax, ecx);
							c.cmova(eax, ecx);
							c.mov(eax, eax);
							break;
						case ValueOpcode::LogicalAnd:
						case ValueOpcode::LogicalOr:
						case ValueOpcode::LogicalXor:
							c.test(rax, rax);
							c.setne(al);
							c.test(rcx, rcx);
							c.setne(cl);
							if (n.op == ValueOpcode::LogicalAnd)
								c.and_(al, cl);
							else if (n.op == ValueOpcode::LogicalOr)
								c.or_(al, cl);
							else
								c.xor_(al, cl);
							c.movzx(eax, al);
							break;
						default: std::abort();
					}
					break;
			}
			c.mov(c.qword[r13 + i * 8], rax);
		}
		c.mov(eax, 1);
		c.jmp(done);
		c.L(fail);
		c.xor_(eax, eax);
		c.L(done);
		c.add(rsp, 8);
		c.pop(r13);
		c.pop(r12);
		c.ret();
		if (Xbyak::GetError() != 0 || c.getSize() > 256 * 1024) {
			Xbyak::ClearError();
			return false;
		}
		c.readyRE();
		if (Xbyak::GetError() != 0) {
			Xbyak::ClearError();
			return false;
		}
		result.function = c.getCode<LinearSrtPlan::Function>();
		return true;
	}
	static bool KYTY_SYSV_ABI ReadUser(const SrtRuntime* runtime, uint64_t index,
	                                   uint64_t* output) {
		if (index >= runtime->user_data.size()) return false;
		*output = runtime->user_data[index];
		return true;
	}
};
#endif
} // namespace Libs::Graphics::ShaderRecompiler::IR
#endif
