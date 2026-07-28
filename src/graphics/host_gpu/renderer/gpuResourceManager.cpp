#include "graphics/host_gpu/renderer/gpuResourceManager.h"

#include "common/assert.h"
#include "common/waitWatch.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"

namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics)
    : m_page_manager(FaultThunk, this), m_buffer_cache(graphics, m_page_manager, m_resource_mutex),
      m_texture_cache(graphics, m_page_manager, m_buffer_cache, m_resource_mutex) {
	m_buffer_cache.SetTextureCache(m_texture_cache);
}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::FaultThunk(void* context, PageFaultAccess access, uint64_t vaddr,
                                    uint64_t size, PageFaultPhase phase) noexcept {
	return static_cast<GpuResourceManager*>(context)->InvalidateMemory(access, vaddr, size, phase);
}

bool GpuResourceManager::InvalidateMemory(PageFaultAccess access, uint64_t vaddr, uint64_t size,
                                          PageFaultPhase phase) noexcept {
	const bool buffer_handled = m_buffer_cache.InvalidateMemory(access, vaddr, size, phase);
	const bool image_handled  = m_texture_cache.InvalidateMemory(access, vaddr, size, phase);
	return buffer_handled || image_handled;
}

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	if (!m_page_manager.IsMapped(fault_vaddr, 1)) {
		return false;
	}
	Kyty::WaitWatch::FaultStats::Record(fault_vaddr);
	// GPU label callbacks run guest code on the dedicated label thread with no label lock held (see
	// LabelManager::ThreadRun, which unlocks before firing). Such a thread is just another non-command-
	// processor faulting thread, so a guest access to GPU-tracked memory can be resolved through the
	// normal non-CP fault path below rather than aborting.
	if (auto* cp = GraphicsRunCurrentCommandProcessor(); cp != nullptr) {
		Kyty::WaitWatch::DrainStats::cp_fault_count.fetch_add(1, std::memory_order_relaxed);
		Kyty::WaitWatch::Scope w("cp_fault", fault_vaddr, static_cast<uint64_t>(access)); // KYTY_DIAG
		cp->BeginReadbackTransaction();
		bool handled = false;
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			handled = m_page_manager.HandleFault(access, fault_vaddr);
		}
		cp->EndReadbackTransaction();
		return handled;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported page fault from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	// A GPU label callback (EOP sync-value write on the dedicated label thread) must NOT pause
	// submissions to resolve its fault. The pause it would need can be held by a thread (Done()/another
	// fault) that is itself blocked waiting for the GPU worker to consume this very label, and the
	// worker in turn blocks in a PM4 handler waiting for the callback's write -> a
	// Done -> worker -> label-callback -> Done deadlock (observed hanging GTA III at level load). A
	// label callback only writes sync values for pipeline work the GPU has already retired (end-of-
	// pipe), so the region is no longer in flight; resolving under the resource mutex alone -- without
	// the submission pause / GPU fence -- is coherent and breaks the cycle.
	if (LabelInCallback()) {
		Kyty::WaitWatch::Scope       w("label_fault", fault_vaddr, 0); // KYTY_DIAG
		ResourceMutex::FaultScope    fault(m_resource_mutex);
		return m_page_manager.HandleFault(access, fault_vaddr);
	}
	// Drain only for sync-relevant faults. The overwhelming majority of level-streaming faults are
	// pure invalidations -- a CPU write to a buffer the GPU has a clean copy of, or a read of a
	// page the GPU has not dirtied -- which download nothing and need neither the worker park nor
	// the GPU fence. Only a fault that will actually read GPU data back (buffer page GPU-dirty, or
	// a texture readback candidate) needs the expensive drain. The probe runs under the resource
	// mutex, which serializes against the GPU worker's resource transactions and label callbacks
	// (the only writers of GPU-dirty state), so it exactly predicts whether HandleFault's
	// Invalidate phase will download -- measured ~5-40% of faults during streaming, the rest now
	// resolve without stalling the GPU. (The old unconditional drain also flushed GPU labels here;
	// that flush still happens on every readback fault, which occur continuously during streaming.)
	{
		ResourceMutex::FaultScope probe(m_resource_mutex);
		if (!m_texture_cache.FaultWouldReadback(fault_vaddr) &&
		    !m_buffer_cache.FaultWouldReadback(fault_vaddr)) {
			return m_page_manager.HandleFault(access, fault_vaddr);
		}
	}
	Kyty::WaitWatch::DrainStats::fault_count.fetch_add(1, std::memory_order_relaxed);
	GraphicsRunSubmissionLock submissions;
	ResourceMutex::FaultScope fault(m_resource_mutex);
	return m_page_manager.HandleFault(access, fault_vaddr);
}

void GpuResourceManager::PrepareHostWrite(uint64_t vaddr, uint64_t size) {
	if (!m_page_manager.HasAnyMapping(vaddr, size)) {
		return;
	}
	if (LabelInCallback()) {
		EXIT("unsupported host write from an asynchronous GPU label callback, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto handle_range = [this, vaddr, size]() {
		if (!m_page_manager.HandleWriteRange(vaddr, size)) {
			EXIT("failed to prepare host write, addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vaddr, size);
		}
	};
	if (auto* cp = GraphicsRunCurrentCommandProcessor(); cp != nullptr) {
		cp->BeginReadbackTransaction();
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			handle_range();
		}
		cp->EndReadbackTransaction();
		return;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported host write from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	GraphicsRunSubmissionLock submissions;
	ResourceMutex::FaultScope fault(m_resource_mutex);
	handle_range();
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	return m_page_manager.IsMapped(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size, GpuAccess access) {
	m_page_manager.OnGpuMap(vaddr, size, access);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size, GpuAccess access) {
	if (!IsMapped(vaddr, size)) {
		EXIT("cannot unmap an unmapped GPU resource range\n");
	}
	m_texture_cache.UnmapMemory(vaddr, size);
	m_buffer_cache.UnmapMemory(vaddr, size);
	m_page_manager.OnGpuUnmap(vaddr, size, access);
}

void GpuResourceManager::FillBuffer(CommandBuffer& command, uint64_t vaddr, uint64_t size,
                                    uint32_t value) {
	if (command.IsInvalid()) {
		EXIT("cannot fill a buffer without a valid render command context\n");
	}
	Common::LockGuard lock(GetRenderContext().GetMutex());
	m_buffer_cache.FillBuffer(&command, vaddr, size, value);
}

void GpuResourceManager::CopyBuffer(CommandBuffer& command, uint64_t dst_vaddr, uint64_t src_vaddr,
                                    uint64_t size) {
	if (command.IsInvalid()) {
		EXIT("cannot copy a buffer without a valid render command context\n");
	}
	Common::LockGuard lock(GetRenderContext().GetMutex());
	m_buffer_cache.CopyBuffer(&command, dst_vaddr, src_vaddr, size);
}

} // namespace Libs::Graphics
