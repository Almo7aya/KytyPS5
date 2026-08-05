#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
namespace Libs::Graphics {

FenceResourceRetainer::~FenceResourceRetainer() {
	if (!m_resources.empty()) {
		EXIT("fence resource retainer destroyed before release\n");
	}
}

void FenceResourceRetainer::Retain(std::shared_ptr<void> resource) {
	if (resource == nullptr) {
		EXIT("cannot retain a null fence resource\n");
	}
	if (std::ranges::none_of(m_resources, [&resource](const auto& retained) {
		    return retained.get() == resource.get();
	    })) {
		m_resources.push_back(std::move(resource));
	}
}

void FenceResourceRetainer::ReleaseAfterFence() noexcept {
	m_resources.clear();
}

CommandBuffer::CommandBuffer(CommandScheduler& scheduler)
    : m_context(scheduler.Context()), m_scheduler(scheduler), m_graphics(scheduler.Graphics()),
      m_slot(scheduler.AllocateCommandBuffer()) {}

CommandBuffer::~CommandBuffer() {
	Release();
}

bool CommandBuffer::IsInvalid() const {
	return m_slot == nullptr;
}

vk::CommandBuffer CommandBuffer::Handle() const {
	EXIT_IF(IsInvalid());

	const auto handle = m_slot->buffer;
	EXIT_IF(handle == nullptr);
	return handle;
}

void CommandBuffer::Release() {
	EXIT_IF(IsInvalid());

	Common::LockGuard lock(*m_slot->pool_mutex);

	WaitForFence();

	m_slot->busy = false;
	m_slot->Reset();
	ReleaseResourcesAfterFence();
	if (m_occlusion_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_occlusion_pool, nullptr);
		m_occlusion_pool = nullptr;
	}
	m_slot = nullptr;

	EXIT_NOT_IMPLEMENTED(!IsInvalid());
}

void CommandBuffer::RetireBufferAfterFence(std::unique_ptr<VulkanBuffer> buffer) {
	if (IsInvalid() || m_execute || buffer == nullptr || buffer->buffer == nullptr) {
		EXIT("cannot retire a buffer on an invalid or submitted command buffer\n");
	}
	m_retired_buffers.push_back(std::move(buffer));
}

void CommandBuffer::RetainResourceUntilFence(std::shared_ptr<void> resource) {
	if (IsInvalid() || m_execute) {
		EXIT("cannot retain a resource on an invalid or submitted command buffer\n");
	}
	m_fence_resources.Retain(std::move(resource));
}

void CommandBuffer::RecycleDescriptorAfterFence(VulkanDescriptorSet& set) {
	m_descriptor_sets_after_fence.push_back(&set);
}

void CommandBuffer::RecycleDescriptorsAfterFence() {
	for (auto* set: m_descriptor_sets_after_fence) {
		m_context.GetDescriptorCache().Recycle(*set);
	}
	m_descriptor_sets_after_fence.clear();
}

// TEMPORARY PROBE: occlusion-query effectiveness. Distinguishes "samples fall back to always-visible"
// from "queries run but nothing is occluded" from "over-draw is not where the cost is".
namespace {
std::atomic<uint64_t> g_zpass_serviced {0};
std::atomic<uint64_t> g_zpass_fallback {0};
std::atomic<uint64_t> g_zpass_occluded {0};
std::atomic<uint64_t> g_zpass_visible {0};
std::atomic<uint32_t> g_zpass_reports {0};

void ProbeReportZPass() {
	const auto serviced = g_zpass_serviced.load(std::memory_order_relaxed);
	if (serviced == 0 || serviced % 4096 != 0) {
		return;
	}
	if (g_zpass_reports.fetch_add(1, std::memory_order_relaxed) >= 16) {
		return;
	}
	std::fprintf(stderr,
	             "[gta3-probe] zpass serviced=%llu fallback=%llu occluded=%llu visible=%llu\n",
	             static_cast<unsigned long long>(serviced),
	             static_cast<unsigned long long>(g_zpass_fallback.load(std::memory_order_relaxed)),
	             static_cast<unsigned long long>(g_zpass_occluded.load(std::memory_order_relaxed)),
	             static_cast<unsigned long long>(g_zpass_visible.load(std::memory_order_relaxed)));
	std::fflush(stderr);
}
} // namespace

bool CommandBuffer::SampleZPassCounter(void* dst_gpu_addr) {
	if (dst_gpu_addr == nullptr || IsInvalid() || m_execute) {
		g_zpass_fallback.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	if (m_occlusion_pool == nullptr) {
		vk::QueryPoolCreateInfo info {};
		info.sType      = vk::StructureType::eQueryPoolCreateInfo;
		info.queryType  = vk::QueryType::eOcclusion;
		info.queryCount = OcclusionQuerySlots;
		if (m_graphics.device.createQueryPool(&info, nullptr, &m_occlusion_pool) !=
		        vk::Result::eSuccess ||
		    m_occlusion_pool == nullptr) {
			m_occlusion_pool = nullptr;
			g_zpass_fallback.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		Handle().resetQueryPool(m_occlusion_pool, 0, OcclusionQuerySlots);
	}

	// Which half of the pair this is has to be decided from the guest's own addressing, not from
	// whether Kyty managed to service the previous sample. The guest brackets a draw with two dumps
	// into consecutive 64-bit slots and takes end-minus-begin, so the end is the dump landing exactly
	// one 64-bit slot above the begin it is armed for.
	//
	// Deciding it from a bare `armed` flag - as this did - desynchronises permanently the first time a
	// begin dump is not serviced: the flag stays clear, the guest's *end* is then taken for a begin,
	// and from there every pair in the recording is mismatched. Kyty writes zero where the guest
	// expects the post-draw count and a query result where it expects the pre-draw count, so
	// end-minus-begin is meaningless and primitives blink in and out for the rest of the frame. The
	// measured fallback rate (13306 of 49152 samples) meant that was happening constantly, and
	// ResetZPassCounters clearing the flag each fence is what made it come and go per frame rather
	// than latch - which is exactly what "models hide and show randomly" looks like.
	const auto address = reinterpret_cast<uint64_t>(dst_gpu_addr);
	if (m_occlusion_armed && address == m_occlusion_armed_address + sizeof(uint64_t)) {
		// End dump: the draw path has already bracketed the query, so bind its slot to this address.
		// If the draw never ran, m_occlusion_last_slot is -1 and the caller falls back.
		const auto slot   = m_occlusion_last_slot;
		m_occlusion_armed = false;
		if (slot < 0) {
			g_zpass_fallback.fetch_add(1, std::memory_order_relaxed);
			ProbeReportZPass();
			return false;
		}
		m_pending_z_pass.push_back({address, slot});
		m_occlusion_last_slot = -1;
		g_zpass_serviced.fetch_add(1, std::memory_order_relaxed);
		ProbeReportZPass();
		return true;
	}

	// Begin dump: the guest reads this as the counter value before the draw, so it resolves to zero
	// and the difference against the end dump is the visible-pixel count. Only arm here; the render
	// pass does not exist yet.
	//
	// Arming happens even with the pool exhausted. BeginArmedZPassQuery will decline to allocate a
	// slot, so the end dump finds none and falls back to always-visible - which costs culling for that
	// primitive but keeps the pairing aligned with the guest, instead of trading one unculled
	// primitive for a whole recording of mismatched pairs.
	m_pending_z_pass.push_back({address, -1});
	m_occlusion_armed         = true;
	m_occlusion_armed_address = address;
	m_occlusion_last_slot     = -1;
	g_zpass_serviced.fetch_add(1, std::memory_order_relaxed);
	ProbeReportZPass();
	return true;
}

bool CommandBuffer::BeginArmedZPassQuery() {
	if (!m_occlusion_armed || m_occlusion_pool == nullptr || !m_rendering || IsInvalid() ||
	    m_occlusion_open_slot >= 0 || m_occlusion_next_slot >= OcclusionQuerySlots) {
		return false;
	}
	m_occlusion_open_slot = static_cast<int32_t>(m_occlusion_next_slot++);
	Handle().beginQuery(m_occlusion_pool, static_cast<uint32_t>(m_occlusion_open_slot),
	                    vk::QueryControlFlags {});
	return true;
}

void CommandBuffer::EndArmedZPassQuery() {
	if (m_occlusion_open_slot < 0) {
		return;
	}
	Handle().endQuery(m_occlusion_pool, static_cast<uint32_t>(m_occlusion_open_slot));
	m_occlusion_last_slot = m_occlusion_open_slot;
	m_occlusion_open_slot = -1;
}

void CommandBuffer::ResolveZPassCounters() {
	if (m_pending_z_pass.empty()) {
		ResetZPassCounters();
		return;
	}

	std::vector<uint64_t> results(m_occlusion_next_slot, 0);
	if (m_occlusion_pool != nullptr && m_occlusion_next_slot != 0) {
		// The fence has already been waited on, so the results are available and this does not stall.
		const auto status = m_graphics.device.getQueryPoolResults(
		    m_occlusion_pool, 0, m_occlusion_next_slot, results.size() * sizeof(uint64_t),
		    results.data(), sizeof(uint64_t),
		    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
		if (status != vk::Result::eSuccess) {
			// Report visible rather than culling on a readback failure.
			std::fill(results.begin(), results.end(), uint64_t {1});
		}
	}

	for (const auto& pending: m_pending_z_pass) {
		uint64_t value = 0;
		if (pending.slot >= 0 && static_cast<size_t>(pending.slot) < results.size()) {
			value = results[static_cast<size_t>(pending.slot)];
			(value == 0 ? g_zpass_occluded : g_zpass_visible).fetch_add(1, std::memory_order_relaxed);
		}
		// This runs from the fence-release path, which can be reached while the buffer cache lock is
		// held. A raw store into write-watched guest memory would fault there and the handler would
		// re-enter the cache, tripping its recursive-acquisition guard. Go through the backing store
		// instead, exactly as BufferCache::PublishDownloads does for GPU readback.
		Libs::LibKernel::Memory::WriteBacking(pending.address, &value, sizeof(value));
	}
	ResetZPassCounters();
}

void CommandBuffer::ResetZPassCounters() {
	m_pending_z_pass.clear();
	m_occlusion_open_slot     = -1;
	m_occlusion_armed         = false;
	m_occlusion_armed_address = 0;
	m_occlusion_last_slot     = -1;
	if (m_occlusion_next_slot != 0 && m_occlusion_pool != nullptr && !IsInvalid()) {
		m_occlusion_next_slot = 0;
	}
}

void CommandBuffer::Begin() const {
	EXIT_IF(m_rendering);
	auto buffer = Handle();

	vk::CommandBufferBeginInfo begin_info {};
	begin_info.sType            = vk::StructureType::eCommandBufferBeginInfo;
	begin_info.pNext            = nullptr;
	begin_info.flags            = {};
	begin_info.pInheritanceInfo = nullptr;

	auto result = buffer.begin(&begin_info);

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	// Slot numbering restarts with each recording, so the pool has to be reset before reuse. This
	// must happen outside a render pass instance, which Begin always is.
	if (m_occlusion_pool != nullptr) {
		buffer.resetQueryPool(m_occlusion_pool, 0, OcclusionQuerySlots);
	}
	m_occlusion_open_slot = -1;
}

void CommandBuffer::End() const {
	EndRendering();
	auto buffer = Handle();

	auto result = buffer.end();

	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0, uint32_t arg1,
                                 uint32_t arg2, uint32_t arg3, uint64_t arg4) {
	m_debug_op        = op;
	m_debug_submit_id = submit_id;
	m_debug_arg0      = arg0;
	m_debug_arg1      = arg1;
	m_debug_arg2      = arg2;
	m_debug_arg3      = arg3;
	m_debug_arg4      = arg4;
}

void CommandBuffer::Execute(const SubmitInfo& submit) {
	EXIT_IF(IsInvalid());
	EXIT_IF(m_execute);
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores > SubmitInfo::MaxSemaphores);

	auto buffer = Handle();
	auto fence  = m_slot->fence;

	vk::TimelineSemaphoreSubmitInfo timeline_info {};
	timeline_info.sType                     = vk::StructureType::eTimelineSemaphoreSubmitInfo;
	timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
	timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
	timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
	timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

	vk::SubmitInfo submit_info {};
	submit_info.sType                = vk::StructureType::eSubmitInfo;
	submit_info.pNext                = &timeline_info;
	submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
	submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
	submit_info.pWaitDstStageMask    = submit.wait_stages.data();
	submit_info.commandBufferCount   = 1;
	submit_info.pCommandBuffers      = &buffer;
	submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
	submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

	auto& graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	auto result = graphics.device.resetFences(1, &fence);
	if (result != vk::Result::eSuccess) {
		LOGF("vkResetFences failed before submit: %s (%d)\n", VulkanToString(result).c_str(),
		     static_cast<int>(result));
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	if (Config::GraphicsDebugDumpEnabled()) {
		LOGF("vkQueueSubmit begin: slot=%u waits=%u signals=%u debug_op=%u debug_submit=%" PRIu64
		     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     m_slot->id, submit.num_wait_semaphores, submit.num_signal_semaphores, m_debug_op,
		     m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}

	{
		Common::LockGuard lock(graphics.queue_mutex);
		m_submit_seq = m_scheduler.NextSubmitSequence();
		result       = graphics.queue.submit(1, &submit_info, fence);
	}

	m_execute      = true;
	m_fence_waited = false;

	if (result != vk::Result::eSuccess) {
		LOGF("vkQueueSubmit failed: %s (%d), slot=%u submit_seq=%" PRIu64
		     " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     VulkanToString(result).c_str(), static_cast<int>(result), m_slot->id, m_submit_seq,
		     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
}

void CommandBuffer::WaitForFence() {
	FinalizeFence(false);
}

void CommandBuffer::WaitForFenceOnly() {
	EXIT_IF(IsInvalid());
	if (!m_execute || m_fence_waited) {
		return;
	}
	auto device = m_graphics.device;
	auto result = device.waitForFences(1, &m_slot->fence, VK_TRUE, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		LOGF("vkWaitForFences failed: %s (%d), slot=%u submit_seq=%" PRIu64
		     " debug_op=%u debug_submit=%" PRIu64 " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
		     VulkanToString(result).c_str(), static_cast<int>(result), m_slot->id, m_submit_seq,
		     m_debug_op, m_debug_submit_id, m_debug_arg0, m_debug_arg1, m_debug_arg2, m_debug_arg3,
		     m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	m_fence_waited = true;
}

void CommandBuffer::WaitForFenceAndReset() {
	FinalizeFence(true);
}

void CommandBuffer::FinalizeFence(bool reset_recording) {
	const bool was_executed = m_execute;
	WaitForFenceOnly();
	if (was_executed) {
		m_execute      = false;
		m_fence_waited = false;
		if (reset_recording) {
			Common::LockGuard lock(*m_slot->pool_mutex);
			m_slot->Reset();
		}
	}
	if (was_executed) {
		ReleaseResourcesAfterFence();
	}
	DeleteBuffersAfterFence();
}

void CommandBuffer::ReleaseResourcesAfterFence() {
	// Publish occlusion results before anything else: the guest polls these addresses.
	ResolveZPassCounters();
	RecycleDescriptorsAfterFence();
	m_fence_resources.ReleaseAfterFence();
}

void CommandBuffer::DeleteBuffersAfterFence() {
	for (const auto& buffer: m_retired_buffers) {
		m_graphics.DeleteBuffer(*buffer);
	}
	m_retired_buffers.clear();
}

void CommandBuffer::BeginRendering(const RenderState& state) const {
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.num_color_attachments > RENDER_COLOR_ATTACHMENTS_MAX);
	if (m_rendering && m_render_state == state) {
		return;
	}
	EndRendering();

	std::array<vk::RenderingAttachmentInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
	for (uint32_t i = 0; i < state.num_color_attachments; i++) {
		const auto& attachment = state.color_attachments[i];
		colors[i].sType        = vk::StructureType::eRenderingAttachmentInfo;
		colors[i].imageView    = attachment.image_view;
		colors[i].imageLayout  = attachment.image_layout;
		colors[i].loadOp =
		    attachment.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colors[i].storeOp                 = vk::AttachmentStoreOp::eStore;
		colors[i].clearValue.color.uint32 = attachment.clear_value;
	}

	const auto&                 depth_stencil = state.depth_stencil_attachment;
	vk::RenderingAttachmentInfo depth {};
	depth.sType       = vk::StructureType::eRenderingAttachmentInfo;
	depth.imageView   = depth_stencil.image_view;
	depth.imageLayout = depth_stencil.image_layout;
	depth.loadOp =
	    depth_stencil.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depth.storeOp                       = vk::AttachmentStoreOp::eStore;
	depth.clearValue.depthStencil.depth = std::bit_cast<float>(depth_stencil.clear_value[0]);

	vk::RenderingAttachmentInfo stencil {};
	stencil.sType       = vk::StructureType::eRenderingAttachmentInfo;
	stencil.imageView   = depth_stencil.image_view;
	stencil.imageLayout = depth_stencil.image_layout;
	stencil.loadOp =
	    depth_stencil.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	stencil.storeOp                         = vk::AttachmentStoreOp::eStore;
	stencil.clearValue.depthStencil.stencil = depth_stencil.clear_value[1];

	vk::RenderingInfo rendering {};
	rendering.sType                = vk::StructureType::eRenderingInfo;
	rendering.renderArea.extent    = {state.width, state.height};
	rendering.layerCount           = state.num_layers;
	rendering.colorAttachmentCount = state.num_color_attachments;
	rendering.pColorAttachments    = colors.data();
	rendering.pDepthAttachment     = depth_stencil.has_depth ? &depth : nullptr;
	rendering.pStencilAttachment   = depth_stencil.has_stencil ? &stencil : nullptr;
	Handle().beginRendering(rendering);
	m_render_state = state;
	m_rendering    = true;
}

void CommandBuffer::EndRendering() const {
	if (!m_rendering) {
		return;
	}
	if (m_occlusion_open_slot >= 0) {
		// A query must not outlive its render pass instance. This should not happen -- the guest
		// brackets a single draw -- but closing it here keeps the command stream valid, and the
		// pending begin entry simply resolves to zero alongside an unwritten result.
		Handle().endQuery(m_occlusion_pool, static_cast<uint32_t>(m_occlusion_open_slot));
		m_occlusion_open_slot = -1;
	}
	Handle().endRendering();
	m_rendering    = false;
	m_render_state = {};
}

} // namespace Libs::Graphics
