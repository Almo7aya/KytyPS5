#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/pipeline/shaderSubgroup.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
static uint64_t BufferDescriptorSize(const ShaderBufferResource& descriptor) {
	const uint64_t records = descriptor.NumRecords();
	const uint64_t stride  = descriptor.Stride();
	if (stride != 0 && records > UINT64_MAX / stride) {
		EXIT("compute buffer descriptor footprint overflow\n");
	}
	return stride == 0 ? records : records * stride;
}

// Recovers the constant a metadata fill stores when it does not read the value from a companion buffer.
//
// A clear to zero needs no companion: the shader just writes an immediate. The read-only-buffer route
// below cannot see that, so the clear was recorded with no code, MetaClearCode then reported none, and
// AcquireRenderTargets took neither the constant-encoded branch nor the clear-register branch - it set no
// clear and consumed the state anyway. GTA III's velocity surface is cleared exactly this way: 456 clears
// in one session, every one recorded with clear_code_valid false and thrown away, which left the buffer
// accumulating the previous frames' silhouettes.
//
// This reads the value the shader actually writes rather than assuming one, which matters because 0x00
// (clear to black) and 0xff (uncompressed, i.e. a decompress that must not become a clear) are both
// plausible constant fills. Ambiguity is rejected: more than one distinct stored immediate returns false
// and the caller keeps today's behaviour.
static bool TryRecoverStoredImmediate(const ShaderRecompiler::IR::Program& program, uint32_t& value) {
	using namespace ShaderRecompiler::IR;

	bool     found  = false;
	uint32_t result = 0;
	for (const auto& block: program.blocks) {
		for (size_t i = 0; i < block.instructions.size(); i++) {
			const auto& store = block.instructions[i];
			if (store.op != Opcode::BufferStoreDword ||
			    store.src[0].kind != OperandKind::Register) {
				continue;
			}
			const auto reg = store.src[0].reg;
			// Walk back to the register's most recent definition. Only a direct immediate move counts;
			// anything else means the value is computed and this analysis does not model it.
			for (size_t j = i; j-- > 0;) {
				const auto& def = block.instructions[j];
				if (def.dst.kind != OperandKind::Register || !(def.dst.reg == reg)) {
					continue;
				}
				if ((def.op == Opcode::MoveU32 || def.op == Opcode::MoveF32Bits) &&
				    def.src[0].kind == OperandKind::ImmediateU32) {
					if (found && result != def.src[0].imm) {
						return false;
					}
					found  = true;
					result = def.src[0].imm;
				}
				break;
			}
		}
	}
	value = result;
	return found;
}

bool RenderExecutor::TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
                                                const RenderCommandBuffer&    buffer) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (resources.buffers.size() != program.info.buffers.size()) {
		EXIT("compute runtime buffer count does not match shader metadata\n");
	}
	auto& cache = buffer.GetContext().GetTextureCache();
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& resource   = program.info.buffers[i];
		const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		if (!resource.written && cache.IsMeta(descriptor.Base48())) {
			return false;
		}
	}

	if (!program.info.has_bitwise_xor) {
		// A metadata clear splats a value sourced from a companion read-only buffer. The recompiled
		// shader adds only Kyty's own host alignment adjustment on top of guest offset 0, so the
		// value itself lives at that descriptor's base address and can be read from guest backing.
		// Recording it matters because a constant-encoded DCC clear code carries the clear colour,
		// while an uncompressed code (0xff) is a decompress that must not become a clear at all.
		uint32_t clear_code       = 0;
		bool     clear_code_valid = false;
		uint32_t read_only_count  = 0;
		uint64_t source_address   = 0;
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			if (!program.info.buffers[i].written) {
				read_only_count++;
				source_address =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]).Base48();
			}
		}
		if (read_only_count == 1 && source_address != 0) {
			uint32_t value = 0;
			if (Libs::LibKernel::Memory::TryReadBacking(source_address, &value, sizeof(value))) {
				const uint32_t code = value & 0xffu;
				if (value == (code | (code << 8u) | (code << 16u) | (code << 24u))) {
					clear_code       = code;
					clear_code_valid = true;
				}
			}
		}
		if (!clear_code_valid) {
			// No companion buffer to read the value from, so the shader writes it as an immediate.
			uint32_t value = 0;
			if (TryRecoverStoredImmediate(program, value)) {
				const uint32_t code = value & 0xffu;
				if (value == (code | (code << 8u) | (code << 16u) | (code << 24u))) {
					clear_code       = code;
					clear_code_valid = true;
				}
			}
		}

		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& resource = program.info.buffers[i];
			if (resource.written) {
				const auto descriptor =
				    DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
				if (cache.ClearMeta(descriptor.Base48(), clear_code, clear_code_valid)) {
					return true;
				}
			}
		}
	}
	return false;
}

bool ResolveComputeImageClear(const ShaderComputeInputInfo& input, uint32_t group_x,
                              uint32_t group_y, uint32_t group_z, uint32_t mode,
                              ShaderBufferResource& resolved_descriptor, uint32_t& resolved_clear,
                              uint64_t& resolved_size) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	if (program.info.buffers.size() != 1 || resources.buffers.size() != 1 ||
	    !program.info.images.empty() || !program.info.samplers.empty() ||
	    !program.info.addresses.empty() || !resources.images.empty() ||
	    !resources.samplers.empty() || !resources.addresses.empty()) {
		return false;
	}
	const auto& resource   = program.info.buffers.front();
	const auto& raw        = resources.buffers.front();
	const auto  descriptor = DecodeNativeDescriptor<ShaderBufferResource>(raw);
	if (!resource.formatted || !resource.written || resource.read || resource.atomic ||
	    resource.scalar || resource.max_byte_extent != 16 || descriptor.Stride() != 16 ||
	    descriptor.Format() != Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32UInt) ||
	    descriptor.SwizzleEnabled() || descriptor.IndexStride() != 0 || descriptor.AddTid() ||
	    resource.packed_stride != descriptor.PackedStride() || raw.dword_count != 4 ||
	    program.user_data_base != 0 || resources.user_data.size() != 8) {
		return false;
	}
	for (uint32_t i = 0; i < raw.dword_count; i++) {
		if (raw.dwords[i] != resources.user_data[i]) {
			return false;
		}
	}
	const uint32_t clear = resources.user_data[4];
	if (resources.user_data[5] != clear || resources.user_data[6] != clear ||
	    resources.user_data[7] != clear) {
		return false;
	}
	const bool full_dispatch =
	    input.dispatch_thread_dimensions && input.threads_num[0] == 64 &&
	    input.threads_num[1] == 1 && input.threads_num[2] == 1 && group_x != 0 && group_y == 1 &&
	    group_z == 1 && input.dispatch_threads_num[0] == group_x &&
	    input.dispatch_threads_num[1] == 1 && input.dispatch_threads_num[2] == 1 &&
	    input.group_id[0] && !input.group_id[1] && !input.group_id[2] &&
	    input.thread_ids_num == 1 && input.wave_size == 32 && !input.tg_size_en && mode == 0x61u &&
	    group_x % input.threads_num[0] == 0 && descriptor.NumRecords() == group_x;
	const auto size = BufferDescriptorSize(descriptor);
	if (!full_dispatch || size == 0) {
		return false;
	}
	resolved_descriptor = descriptor;
	resolved_clear      = clear;
	resolved_size       = size;
	return true;
}

static bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input, CommandBuffer& command,
                                        uint32_t group_x, uint32_t group_y, uint32_t group_z,
                                        uint32_t mode) {
	ShaderBufferResource descriptor;
	uint32_t             packed_clear = 0;
	uint64_t             size         = 0;
	if (!ResolveComputeImageClear(input, group_x, group_y, group_z, mode, descriptor, packed_clear,
	                              size)) {
		return false;
	}
	auto& cache = command.GetContext().GetTextureCache();
	if (!cache.ClearImageFromBuffer(command, descriptor.Base48(), size, packed_clear)) {
		return false;
	}
	static std::atomic<uint32_t> logged_clears {0};
	if (logged_clears.fetch_add(1, std::memory_order_relaxed) < 32) {
		LOGF("GraphicsRenderDispatchDirect: compute image clear shader=0x%016" PRIx64
		     " addr=0x%016" PRIx64 " size=0x%016" PRIx64 " value=0x%08" PRIx32 "\n",
		     input.stage.program->shader_hash, descriptor.Base48(), size, packed_clear);
	}
	return true;
}

void RenderExecutor::DispatchDirect(uint64_t submit_id, RenderCommandBuffer& buffer,
                                    uint32_t thread_group_x, uint32_t thread_group_y,
                                    uint32_t thread_group_z, uint32_t mode) {
	EXIT_IF(buffer.IsInvalid());
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DispatchDirect), submit_id,
	                    thread_group_x, thread_group_y, thread_group_z, mode,
	                    sh_ctx.GetCs().cs_regs.data_addr);

	Common::LockGuard lock(m_context.GetMutex());
	if (sh_ctx.GetCs().cs_regs.data_addr == 0) {
		LOGF("GraphicsRenderDispatchDirect: temporary: ignoring dispatch with null CS shader, "
		     "groups=%ux%ux%u mode=%u\n",
		     thread_group_x, thread_group_y, thread_group_z, mode);
		return;
	}

	if (!ShaderAddressValid(sh_ctx.GetCs().cs_regs.data_addr)) {
		return;
	}

	constexpr uint32_t DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS = 1u << 5u;
	constexpr uint32_t DISPATCH_INITIATOR_BASE_BITS             = 0x41u;
	constexpr uint32_t DISPATCH_INITIATOR_MODIFIER_BITS         = 0xa038u;
	constexpr uint32_t DISPATCH_INITIATOR_KNOWN_MASK =
	    DISPATCH_INITIATOR_BASE_BITS | DISPATCH_INITIATOR_MODIFIER_BITS;

	const uint32_t unknown_mode_bits = mode & ~DISPATCH_INITIATOR_KNOWN_MASK;
	if (unknown_mode_bits != 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: unknown dispatch initiator bits "
			     "mode=0x%08" PRIx32 " unknown=0x%08" PRIx32 " shader=0x%016" PRIx64
			     " groups=%ux%ux%u\n",
			     mode, unknown_mode_bits, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x,
			     thread_group_y, thread_group_z);
		}
	}

	const auto& cs_regs = sh_ctx.GetCs();
	const auto& sh_regs = ctx.GetShaderRegisters();

	ShaderComputeInputInfo    input_info {};
	std::span<const uint32_t> cs_shader;
	if (!ShaderCompileInfoCS(cs_regs, sh_regs, input_info, cs_shader)) {
		EXIT("ShaderCompileInfoCS failed for dispatch with CS shader 0x%016" PRIx64 "\n",
		     cs_regs.cs_regs.data_addr);
	}

	const bool use_thread_dimensions = (mode & DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS) != 0;
	if (use_thread_dimensions) {
		input_info.dispatch_thread_dimensions = true;
		input_info.dispatch_threads_num[0]    = thread_group_x;
		input_info.dispatch_threads_num[1]    = thread_group_y;
		input_info.dispatch_threads_num[2]    = thread_group_z;
	}

	const uint32_t frame_num = static_cast<uint32_t>(m_context.GetGpu().GetFrameNum());
	const bool     large_workgroup =
	    (input_info.threads_num[0] * input_info.threads_num[1] * input_info.threads_num[2] >= 512);
	const auto& program   = *input_info.stage.program;
	const auto& resources = *input_info.stage.resources;
	if (TryConsumeComputeMetaClear(input_info, buffer)) {
		ResetBindings();
		return;
	}
	if (TryConsumeComputeImageClear(input_info, buffer, thread_group_x, thread_group_y,
	                                thread_group_z, mode)) {
		ResetBindings();
		return;
	}
	const auto sampled_images = std::count_if(
	    program.info.images.begin(), program.info.images.end(), [](const auto& image) {
		    return image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
		           image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint;
	    });
	const bool                   has_sampler = !program.info.samplers.empty();
	static std::atomic<uint32_t> dispatch_log_count {0};
	if ((large_workgroup || has_sampler) &&
	    dispatch_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
		LOGF("GraphicsRenderDispatchDirect: frame=%u shader=0x%016" PRIx64
		     " groups=%ux%ux%u mode=0x%08" PRIx32 " local=%ux%ux%u "
		     "buffers=%zu textures=%zu sampled=%zu storage=%zu samplers=%zu push=%u\n",
		     frame_num, sh_ctx.GetCs().cs_regs.data_addr, thread_group_x, thread_group_y,
		     thread_group_z, mode, input_info.threads_num[0], input_info.threads_num[1],
		     input_info.threads_num[2], program.info.buffers.size(), program.info.images.size(),
		     sampled_images, program.info.images.size() - sampled_images,
		     program.info.samplers.size(), program.bindings.push_constant_size);
		for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
			const auto& buffer = program.info.buffers[i];
			const auto  r      = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
			LOGF("  CS buffer[%u]: source=%u usage=%s addr=0x%012" PRIx64
			     " stride=%u records=%u format=%u\n",
			     i, buffer.source, buffer.written ? "read-write" : "read-only", r.Base48(),
			     r.Stride(), r.NumRecords(), r.Format());
		}
		for (uint32_t i = 0; i < program.info.images.size(); i++) {
			const auto& image = program.info.images[i];
			const auto  r     = DecodeNativeDescriptor<ShaderTextureResource>(resources.images[i]);
			LOGF("  CS texture[%u]: source=%u usage=%s sampled=%s addr=0x%010" PRIx64
			     " type=%u fmt=%u extent=%ux%u depth=%u levels=%u tile=%u\n",
			     i, image.source, image.written ? "read-write" : "read-only",
			     (image.kind == ShaderRecompiler::IR::ResourceKind::Image ||
			      image.kind == ShaderRecompiler::IR::ResourceKind::ImageUint)
			         ? "true"
			         : "false",
			     r.Base40(), static_cast<uint32_t>(r.Type()), r.Format(),
			     static_cast<uint32_t>(r.Width5()) + 1u, static_cast<uint32_t>(r.Height5()) + 1u,
			     static_cast<uint32_t>(r.Depth()) + 1u,
			     std::max<uint32_t>(static_cast<uint32_t>(r.LastLevel()),
			                        static_cast<uint32_t>(r.MaxMip())) +
			         1u,
			     r.TileMode());
		}
		for (uint32_t i = 0; i < program.info.samplers.size(); i++) {
			const auto r = DecodeNativeDescriptor<ShaderSamplerResource>(resources.samplers[i]);
			LOGF("  CS sampler[%u]: source=%u clamp=%u/%u/%u filter=%u/%u/%u mip=%u "
			     "lod=%u-%u bias=%d\n",
			     i, program.info.samplers[i].source, static_cast<uint32_t>(r.ClampX()),
			     static_cast<uint32_t>(r.ClampY()), static_cast<uint32_t>(r.ClampZ()),
			     static_cast<uint32_t>(r.XyMagFilter()), static_cast<uint32_t>(r.XyMinFilter()),
			     static_cast<uint32_t>(r.ZFilter()), static_cast<uint32_t>(r.MipFilter()),
			     static_cast<uint32_t>(r.MinLod()), static_cast<uint32_t>(r.MaxLod()),
			     static_cast<int32_t>(r.LodBias()));
		}
	}

	if (use_thread_dimensions) {
		auto groups_from_threads = [](uint32_t threads, uint32_t group_size) {
			return (threads == 0
			            ? 0u
			            : (threads + std::max(group_size, 1u) - 1u) / std::max(group_size, 1u));
		};

		const uint32_t old_x = thread_group_x;
		const uint32_t old_y = thread_group_y;
		const uint32_t old_z = thread_group_z;
		thread_group_x       = groups_from_threads(thread_group_x, cs_regs.cs_regs.num_thread_x);
		thread_group_y       = groups_from_threads(thread_group_y, cs_regs.cs_regs.num_thread_y);
		thread_group_z       = groups_from_threads(thread_group_z, cs_regs.cs_regs.num_thread_z);

		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: use-thread-dimensions %ux%ux%u / %ux%ux%u -> "
			     "groups %ux%ux%u\n",
			     old_x, old_y, old_z, std::max(cs_regs.cs_regs.num_thread_x, 1u),
			     std::max(cs_regs.cs_regs.num_thread_y, 1u),
			     std::max(cs_regs.cs_regs.num_thread_z, 1u), thread_group_x, thread_group_y,
			     thread_group_z);
		}
	}

	if (thread_group_x == 0 || thread_group_y == 0 || thread_group_z == 0) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("GraphicsRenderDispatchDirect: skipping zero-sized dispatch groups=%ux%ux%u "
			     "mode=0x%08" PRIx32 " shader=0x%016" PRIx64 "\n",
			     thread_group_x, thread_group_y, thread_group_z, mode,
			     sh_ctx.GetCs().cs_regs.data_addr);
		}
		return;
	}

	buffer.EndRendering();
	auto& pipeline =
	    m_context.GetPipelineCache().CreateComputePipeline(input_info, sh_ctx.GetCs(), cs_shader);
	auto bindings = PrepareBindings(buffer, input_info.stage, vk::ShaderStageFlagBits::eCompute,
	                                DescriptorCache::Stage::Compute);
	RebindBuffers(buffer, bindings);
	RebindImages(buffer, bindings);

	auto vk_buffer = buffer.Handle();
	CommitBindings(buffer, vk::PipelineBindPoint::eCompute, pipeline.pipeline_layout, bindings);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline);
	vk_buffer.dispatch(thread_group_x, thread_group_y, thread_group_z);

	bool has_storage_writes = HasShaderBufferWrites(input_info.stage);
	has_storage_writes =
	    std::any_of(program.info.images.begin(), program.info.images.end(),
	                [](const auto& image) {
		                return image.written &&
		                       (image.kind == ShaderRecompiler::IR::ResourceKind::StorageImage ||
		                        image.kind == ShaderRecompiler::IR::ResourceKind::StorageImageUint);
	                }) ||
	    has_storage_writes;
	if (has_storage_writes) {
		ShaderWriteBarrier(vk_buffer, vk::PipelineStageFlagBits::eComputeShader);
	}
	ResetBindings();
}

} // namespace Libs::Graphics
