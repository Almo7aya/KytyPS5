#include "graphics/shader/recompiler/emitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t EmitExportComponentF32(EmitterState& state, const IR::Instruction& inst,
                                uint32_t component) {
	const bool enabled = ((inst.export_info.en >> component) & 1u) != 0;
	if (!enabled || component >= inst.src_count || component >= 4u) {
		return ConstantF32(state, component == 3u ? 0x3f800000u : 0u);
	}
	return EmitFloatLoad(state, inst.src[component]);
}

uint32_t EmitExportVec4F32(EmitterState& state, const IR::Instruction& inst) {
	if (inst.export_info.compr) {
		uint32_t components[4] = {
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0u),
		    ConstantF32(state, 0x3f800000u),
		};
		for (uint32_t pair_index = 0; pair_index < 2u && pair_index < inst.src_count;
		     pair_index++) {
			const auto raw      = EmitValueLoad(state, inst.src[pair_index]);
			const auto unpacked = state.builder.AllocateId();
			state.builder.AddFunction({OpExtInst, state.vec2_float_type, unpacked,
			                           state.glsl_std450, GlslUnpackHalf2x16, raw});
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair_index * 2u + lane;
				if (((inst.export_info.en >> component) & 1u) == 0) {
					continue;
				}
				components[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    {OpCompositeExtract, state.float_type, components[component], unpacked, lane});
			}
		}
		const auto vec = state.builder.AllocateId();
		state.builder.AddFunction({OpCompositeConstruct, state.vec4_float_type, vec, components[0],
		                           components[1], components[2], components[3]});
		return vec;
	}

	const auto x   = EmitExportComponentF32(state, inst, 0);
	const auto y   = EmitExportComponentF32(state, inst, 1);
	const auto z   = EmitExportComponentF32(state, inst, 2);
	const auto w   = EmitExportComponentF32(state, inst, 3);
	const auto vec = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, state.vec4_float_type, vec, x, y, z, w});
	return vec;
}

uint32_t EmitExportComponentU32(EmitterState& state, const IR::Instruction& inst,
                                uint32_t component) {
	const bool enabled = ((inst.export_info.en >> component) & 1u) != 0;
	if (!enabled || component >= inst.src_count || component >= 4u) {
		return ConstantU32(state, component == 3u ? 1u : 0u);
	}
	return EmitValueLoad(state, inst.src[component]);
}

uint32_t EmitExportVec4U32(EmitterState& state, const IR::Instruction& inst) {
	uint32_t components[4] = {
	    ConstantU32(state, 0u),
	    ConstantU32(state, 0u),
	    ConstantU32(state, 0u),
	    ConstantU32(state, 1u),
	};

	if (inst.export_info.compr) {
		for (uint32_t pair_index = 0; pair_index < 2u && pair_index < inst.src_count;
		     pair_index++) {
			const auto raw = EmitValueLoad(state, inst.src[pair_index]);
			for (uint32_t lane = 0; lane < 2u; lane++) {
				const auto component = pair_index * 2u + lane;
				if (((inst.export_info.en >> component) & 1u) == 0) {
					continue;
				}
				components[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    {OpBitFieldUExtract, state.uint_type, components[component], raw,
				     ConstantU32(state, lane * 16u), ConstantU32(state, 16u)});
			}
		}
	} else {
		for (uint32_t component = 0; component < 4u; component++) {
			components[component] = EmitExportComponentU32(state, inst, component);
		}
	}

	const auto vec = state.builder.AllocateId();
	state.builder.AddFunction({OpCompositeConstruct, state.vec4_uint_type, vec, components[0],
	                           components[1], components[2], components[3]});
	return vec;
}

static bool MrtUsesUintOutput(const EmitterState& state, const IR::Instruction& inst) {
	return inst.export_info.kind == IR::ExportTargetKind::Mrt &&
	       state.pixel_input_info != nullptr &&
	       inst.export_info.index < std::size(state.pixel_input_info->target_output_mode) &&
	       state.pixel_input_info->target_output_mode[inst.export_info.index] == 7u;
}

uint32_t ApplyMrtExportMapping(EmitterState& state, const IR::Instruction& inst, uint32_t value,
                               uint32_t vector_type) {
	if (inst.export_info.kind != IR::ExportTargetKind::Mrt || state.pixel_input_info == nullptr ||
	    inst.export_info.index >= state.pixel_input_info->target_export_mapping.size()) {
		return value;
	}

	const auto mapping = state.pixel_input_info->target_export_mapping[inst.export_info.index];
	if (mapping.IsIdentity()) {
		return value;
	}

	const auto mapped = state.builder.AllocateId();
	state.builder.AddFunction({OpVectorShuffle, vector_type, mapped, value, value, mapping.Map(0),
	                           mapping.Map(1), mapping.Map(2), mapping.Map(3)});
	return mapped;
}

bool ExportWritesData(const IR::Instruction& inst) {
	switch (inst.export_info.kind) {
		case IR::ExportTargetKind::Null:
		case IR::ExportTargetKind::Primitive: return false;
		case IR::ExportTargetKind::MrtZ: return (inst.export_info.en & 0x5u) != 0;
		default: return inst.export_info.en != 0;
	}
}

void EmitMrtZExport(EmitterState& state, const IR::Instruction& inst) {
	if ((inst.export_info.en & 0x1u) != 0 && state.depth_variable != 0) {
		state.builder.AddFunction(
		    {OpStore, state.depth_variable, EmitExportComponentF32(state, inst, 0)});
	}

	if ((inst.export_info.en & 0x4u) != 0 && state.sample_mask_variable != 0) {
		const auto raw =
		    inst.src_count > 2u ? EmitValueLoad(state, inst.src[2]) : ConstantU32(state, 0);
		const auto mask = state.builder.AllocateId();
		const auto ptr  = state.builder.AllocateId();
		state.builder.AddFunction({OpBitcast, state.int_type, mask, raw});
		state.builder.AddFunction({OpAccessChain, state.ptr_output_int, ptr,
		                           state.sample_mask_variable, ConstantU32(state, 0)});
		state.builder.AddFunction({OpStore, ptr, mask});
	}
}

// The render-target slice index, exported as POS1.z by the hardware. Only reachable through a
// component-store export - a real POS1 export carries point size and viewport index in its other
// components, which are not modelled, so OutputVariableForExport drops it.
static void EmitLayerExport(EmitterState& state, const IR::Instruction& inst) {
	if (state.layer_variable == 0 || (inst.export_info.en & 0x4u) == 0) {
		return;
	}
	const auto raw   = EmitValueLoad(state, inst.src[2]);
	const auto layer = state.builder.AllocateId();
	state.builder.AddFunction({OpBitcast, state.int_type, layer, raw});
	state.builder.AddFunction({OpStore, state.layer_variable, layer});
}

// Stores only the components `en` selects, leaving the rest of the output as it was. Used when a
// lowering pass has split one hardware export into several instructions, so filling in the disabled
// components - which is right for a real export - would clobber the sibling stores.
static void EmitComponentStoreExport(EmitterState& state, const IR::Instruction& inst,
                                     uint32_t variable) {
	const bool position = inst.export_info.kind == IR::ExportTargetKind::Position;
	for (uint32_t component = 0; component < 4u; component++) {
		if (((inst.export_info.en >> component) & 1u) == 0 || component >= inst.src_count) {
			continue;
		}
		const auto value   = EmitFloatLoad(state, inst.src[component]);
		const auto pointer = state.builder.AllocateId();
		if (position) {
			state.builder.AddFunction({OpAccessChain, state.ptr_output_float, pointer, variable,
			                           ConstantU32(state, 0), ConstantU32(state, component)});
		} else {
			state.builder.AddFunction({OpAccessChain, state.ptr_output_float, pointer, variable,
			                           ConstantU32(state, component)});
		}
		state.builder.AddFunction({OpStore, pointer, value});
	}
}

void EmitExport(EmitterState& state, const IR::Instruction& inst) {
	if (inst.export_info.kind == IR::ExportTargetKind::Null ||
	    inst.export_info.kind == IR::ExportTargetKind::Primitive) {
		return;
	}

	if (!ExportWritesData(inst)) {
		return;
	}

	if (inst.export_info.kind == IR::ExportTargetKind::MrtZ) {
		EmitMrtZExport(state, inst);
		return;
	}

	if (inst.export_info.component_store &&
	    inst.export_info.kind == IR::ExportTargetKind::Position && inst.export_info.index == 1) {
		EmitLayerExport(state, inst);
		return;
	}

	const auto variable = OutputVariableForExport(state, inst.export_info);
	if (variable == 0) {
		return;
	}

	if (inst.export_info.component_store) {
		EmitComponentStoreExport(state, inst, variable);
		return;
	}

	const auto uint_output = MrtUsesUintOutput(state, inst);
	const auto vector_type = uint_output ? state.vec4_uint_type : state.vec4_float_type;
	const auto value       = ApplyMrtExportMapping(
	    state, inst, uint_output ? EmitExportVec4U32(state, inst) : EmitExportVec4F32(state, inst),
	    vector_type);
	if (inst.export_info.kind == IR::ExportTargetKind::Position) {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(
		    {OpAccessChain, state.ptr_output_vec4_float, pointer, variable, ConstantU32(state, 0)});
		state.builder.AddFunction({OpStore, pointer, value});
		return;
	}

	state.builder.AddFunction({OpStore, variable, value});
}

bool ExportUsesPixelValidMask(const EmitterState& state, const IR::Instruction& inst) {
	return state.stage == ShaderType::Pixel && inst.export_info.vm && state.needs_pixel_valid_mask;
}

void EmitKillIfBoolFalse(EmitterState& state, uint32_t active) {
	const auto kill_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	const auto inactive    = state.builder.AllocateId();
	state.builder.AddFunction({OpLogicalNot, state.bool_type, inactive, active});
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, inactive, kill_label, merge_label});
	state.builder.AddFunction({OpLabel, kill_label});
	state.builder.AddFunction({OpKill});
	state.builder.AddFunction({OpLabel, merge_label});
}

// A/B switches for the pixel valid mask, which decides whether the fragment is killed on return.
//
// The mask starts at 1 and is *overwritten* here with `exec ? 1 : 0` at every export carrying the vm bit, so
// only the last such export decides the pixel's fate. In per-invocation mask mode Kyty emulates divergent
// control flow by executing both sides and masking the writes, which means `exec` can legitimately read 0 for
// an invocation sitting in a branch it did not take. A vm export emitted in that state latches the mask to 0
// and OpKill then discards *every* GBuffer output for a pixel that should have lived - a black character and
// missing parts of the car, varying per pixel with the material's branches, while flat road geometry with no
// branching is untouched. See docs section 4.1l.
//
//   KYTY_VM_KILL_STICKY=1 - accumulate validity instead of overwriting: a pixel seen active at any vm export
//                           stays valid. Closer to the hardware's accumulating valid mask.
//   KYTY_NO_VM_KILL=1     - never kill on the valid mask. Blunt: genuine discards (alpha-masked foliage) stop
//                           being discarded, so expect foliage to turn into opaque quads. Useful to confirm
//                           this path is responsible even though it is not shippable.
static bool VmKillSticky() {
	static const bool enabled = std::getenv("KYTY_VM_KILL_STICKY") != nullptr;
	return enabled;
}

static bool NoVmKill() {
	static const bool enabled = std::getenv("KYTY_NO_VM_KILL") != nullptr;
	return enabled;
}

void EmitUpdatePixelValidMask(EmitterState& state) {
	if (state.pixel_valid_mask_variable == 0) {
		return;
	}

	const auto active = EmitExecActiveBool(state);
	const auto value  = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpSelect, state.uint_type, value, active, ConstantU32(state, 1), ConstantU32(state, 0)});
	if (VmKillSticky()) {
		const auto previous = state.builder.AllocateId();
		const auto merged   = state.builder.AllocateId();
		state.builder.AddFunction({OpLoad, state.uint_type, previous, state.pixel_valid_mask_variable});
		state.builder.AddFunction({OpBitwiseOr, state.uint_type, merged, previous, value});
		state.builder.AddFunction({OpStore, state.pixel_valid_mask_variable, merged});
		return;
	}
	state.builder.AddFunction({OpStore, state.pixel_valid_mask_variable, value});
}

void EmitKillIfPixelValidMaskInactive(EmitterState& state) {
	if (state.pixel_valid_mask_variable == 0 || NoVmKill()) {
		return;
	}

	const auto mask_value = state.builder.AllocateId();
	const auto active     = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpLoad, state.uint_type, mask_value, state.pixel_valid_mask_variable});
	state.builder.AddFunction(
	    {OpINotEqual, state.bool_type, active, mask_value, ConstantU32(state, 0)});
	EmitKillIfBoolFalse(state, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
