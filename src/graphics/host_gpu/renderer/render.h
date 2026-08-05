#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <memory>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct ShaderBufferResource;
struct ShaderComputeInputInfo;
struct CommandSlot;
struct VulkanBuffer;
struct VulkanDescriptorSet;
struct RenderDepthInfo;
struct RenderColorInfo;
struct DrawCallInfo;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
struct DrawRenderState;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	EopWrite,
	EopInterrupt,
	EopWriteBack,
	EopFlip,
	EopWriteBackFlip,
	EopOnlyFlip,
	Unknown,
};

class FenceResourceRetainer {
public:
	FenceResourceRetainer() = default;
	~FenceResourceRetainer();
	KYTY_CLASS_NO_COPY(FenceResourceRetainer);

	void               Retain(std::shared_ptr<void> resource);
	void               ReleaseAfterFence() noexcept;
	[[nodiscard]] bool Empty() const noexcept { return m_resources.empty(); }

private:
	std::vector<std::shared_ptr<void>> m_resources;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	explicit CommandBuffer(CommandScheduler& scheduler);
	~CommandBuffer();

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;

	void Begin() const;
	void End() const;
	void Execute(const SubmitInfo& submit = {});
	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	void BeginRendering(const RenderState& state) const;
	void EndRendering() const;
	void WaitForFenceOnly();
	void WaitForFence();
	void WaitForFenceAndReset();
	void RetireBufferAfterFence(std::unique_ptr<VulkanBuffer> buffer);
	void RetainResourceUntilFence(std::shared_ptr<void> resource);
	void RecycleDescriptorAfterFence(VulkanDescriptorSet& set);

	[[nodiscard]] vk::CommandBuffer Handle() const;
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] bool              IsExecute() const { return m_execute; }

	// Emulates the hardware Z-pass counter that PixelPipeStatDump samples. The guest brackets a
	// single occlusion-test draw with two dumps and takes the difference, so the begin dump records 0
	// and the matching end dump records the query result.
	//
	// The dumps cannot drive the query directly: Kyty opens the render pass lazily at draw time, so
	// no render pass instance exists yet when the begin dump arrives. The begin dump therefore only
	// arms the query, the draw path brackets it, and the end dump collects the slot. Returns false
	// when the sample cannot be serviced, so the caller reports the primitive visible rather than
	// silently culling it.
	[[nodiscard]] bool SampleZPassCounter(void* dst_gpu_addr);
	[[nodiscard]] bool BeginArmedZPassQuery();
	void               EndArmedZPassQuery();

private:
	void Release();
	void ResolveZPassCounters();
	void ResetZPassCounters();
	void FinalizeFence(bool reset_recording);
	void ReleaseResourcesAfterFence();
	void DeleteBuffersAfterFence();
	void RecycleDescriptorsAfterFence();

	RenderContext&                             m_context;
	CommandScheduler&                          m_scheduler;
	GraphicContext&                            m_graphics;
	CommandSlot*                               m_slot            = nullptr;
	bool                                       m_execute         = false;
	bool                                       m_fence_waited    = false;
	uint64_t                                   m_submit_seq      = 0;
	uint32_t                                   m_debug_op        = 0;
	uint64_t                                   m_debug_submit_id = 0;
	uint32_t                                   m_debug_arg0      = 0;
	uint32_t                                   m_debug_arg1      = 0;
	uint32_t                                   m_debug_arg2      = 0;
	uint32_t                                   m_debug_arg3      = 0;
	uint64_t                                   m_debug_arg4      = 0;
	std::vector<std::unique_ptr<VulkanBuffer>> m_retired_buffers;
	FenceResourceRetainer                      m_fence_resources;
	std::vector<VulkanDescriptorSet*>          m_descriptor_sets_after_fence;
	mutable RenderState                        m_render_state;
	mutable bool                               m_rendering = false;

	// Occlusion query emulation. One slot per guest query; a pending entry with slot < 0 is the
	// begin dump of a pair and resolves to zero.
	static constexpr uint32_t OcclusionQuerySlots = 2048;

	struct PendingZPass {
		uint64_t address = 0;
		int32_t  slot    = -1;
	};

	vk::QueryPool   m_occlusion_pool      = nullptr;
	uint32_t        m_occlusion_next_slot = 0;
	mutable int32_t m_occlusion_open_slot = -1;
	bool            m_occlusion_armed     = false;
	// Address of the begin dump this query is armed for. The guest writes the pair to consecutive
	// 64-bit slots, so the end dump is identifiable by its address rather than by Kyty's own state -
	// which is what keeps a skipped sample from inverting the pairing for the rest of the recording.
	uint64_t                  m_occlusion_armed_address = 0;
	int32_t                   m_occlusion_last_slot     = -1;
	std::vector<PendingZPass> m_pending_z_pass;
};

class RenderCommandBuffer final: public CommandBuffer {
public:
	explicit RenderCommandBuffer(CommandScheduler& scheduler): CommandBuffer(scheduler) {}

	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	[[nodiscard]] HW::Context&    GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig& GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&     GetShaders() const noexcept { return *m_shaders; }

private:
	HW::Context*    m_registers   = nullptr;
	HW::UserConfig* m_user_config = nullptr;
	HW::Shader*     m_shaders     = nullptr;
};

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context): m_context(context) {}
	KYTY_CLASS_NO_COPY(RenderExecutor);

	void DrawIndex(uint64_t submit_id, RenderCommandBuffer& buffer, uint32_t index_type_and_size,
	               uint32_t index_count, const void* index_addr, uint32_t flags, uint32_t type,
	               uint32_t instance_count = 1, uint32_t render_target_slice_offset = 0,
	               int32_t vertex_offset_add = 0, uint32_t first_instance = 0);
	void DrawAuto(uint64_t submit_id, RenderCommandBuffer& buffer, uint32_t index_count,
	              uint32_t flags, uint32_t render_target_slice_offset = 0,
	              uint32_t instance_count = 1, uint32_t first_vertex = 0,
	              uint32_t first_instance = 0);
	void DispatchDirect(uint64_t submit_id, RenderCommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode);

	[[nodiscard]] DescriptorCache::PreparedBindings
	     PrepareBindings(CommandBuffer& buffer, const ShaderStageRuntime& runtime,
	                     vk::ShaderStageFlags shader_stage, DescriptorCache::Stage stage);
	void RebindBuffers(CommandBuffer& buffer, DescriptorCache::PreparedBindings& bindings);
	void RebindImages(CommandBuffer& buffer, DescriptorCache::PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    vk::PipelineLayout layout, DescriptorCache::PreparedBindings& bindings);

private:
	struct GraphicsBindings {
		DescriptorCache::PreparedBindings                vertex;
		std::optional<DescriptorCache::PreparedBindings> pixel;
	};

	[[nodiscard]] DescriptorCache::TextureBinding
	ResolveTexture(const ShaderRecompiler::IR::ImageResource&   resource,
	               const ShaderRecompiler::IR::DescriptorValue& value);
	[[nodiscard]] GraphicsBindings PrepareGraphicsBindings(CommandBuffer&            buffer,
	                                                       const ShaderStageRuntime& vertex,
	                                                       const ShaderStageRuntime& pixel,
	                                                       bool                      pixel_active);
	void ResolveRenderColorTarget(uint64_t submit_id, RenderCommandBuffer& buffer,
	                              RenderColorInfo& target, uint32_t render_target_slice_offset = 0,
	                              uint32_t render_target_slot = UINT32_MAX,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(uint64_t submit_id, RenderCommandBuffer& buffer,
	                              RenderDepthInfo& target);
	[[nodiscard]] bool PrepareDrawRenderState(uint64_t submit_id, RenderCommandBuffer& buffer,
	                                          const DrawCallInfo& draw,
	                                          uint32_t            render_target_slice_offset,
	                                          bool log_setup_phases, DrawRenderState& state);
	void ExecutePreparedDraw(uint64_t submit_id, RenderCommandBuffer& buffer,
	                         const DrawCallInfo& draw, DrawRenderState& state,
	                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
	                         const DrawIndexBufferSource& index_source, bool log_pipeline_phase,
	                         bool set_bind_debug, bool set_auto_debug);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth);
	[[nodiscard]] bool        ResolveColorTargets(uint64_t submit_id, RenderCommandBuffer& buffer,
	                                              uint32_t render_target_slice_offset);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      TrackImageBinding(ImageId id);
	void                      ResetBindings();
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const RenderCommandBuffer&    buffer);

	RenderContext&                      m_context;
	std::vector<std::shared_ptr<Image>> m_bound_images;

	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeImageClear(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
