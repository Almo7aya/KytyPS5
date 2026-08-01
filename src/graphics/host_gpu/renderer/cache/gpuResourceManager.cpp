#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "common/waitWatch.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache, m_resource_mutex),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache, m_resource_mutex) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	Kyty::WaitWatch::Scope watch("gpu_fault", fault_vaddr,
	                             static_cast<uint64_t>(access)); // KYTY_DIAG
	constexpr uint64_t     fault_size = 8;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	Kyty::WaitWatch::FaultStats::Record(fault_vaddr); // KYTY_DIAG

	// Resolve a window around the fault instead of only the 8 bytes that trapped. A fault raised on
	// a thread that is not the command processor costs a full round-trip to the GPU worker, and the
	// guest rewrites the same streaming buffers every frame: GTA III level load measured ~415k
	// faults over only ~13.7k distinct pages (~30 per page, roughly one per frame). Page-at-a-time
	// resolution therefore pays that round-trip once per 4 KiB per frame, which is what pins the
	// loader near 1 fps. Widening trades a coarser CPU-dirty granularity -- the GPU re-uploads
	// somewhat more per touched region -- for an order of magnitude fewer round-trips. The window is
	// clamped to the contiguous mapped extent so it never reaches into a neighbouring mapping.
	// 64 KiB measured best: 256 KiB over-invalidates (it drags unrelated textures through the
	// texture-cache invalidate) without improving the stall rate.
	constexpr uint64_t fault_window  = 64ull * 1024ull;
	uint64_t           resolve_addr  = fault_vaddr & ~(TRACKER_PAGE_SIZE - 1);
	uint64_t           resolve_size  = MappedExtent(resolve_addr, fault_window);
	if (resolve_size < fault_vaddr - resolve_addr + fault_size) {
		resolve_addr = fault_vaddr;
		resolve_size = fault_size;
	}

	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported guest-memory fault from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	bool       handled = false;
	const auto resolve = [this, access, resolve_addr, resolve_size, &handled](CommandProcessor& cp) {
		cp.BeginReadbackTransaction();
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			if (access == PageFaultAccess::Write) {
				m_buffer_cache.InvalidateMemory(resolve_addr, resolve_size);
				m_texture_cache.InvalidateMemory(resolve_addr, resolve_size);
			} else {
				m_buffer_cache.ReadMemory(resolve_addr, resolve_size);
			}
			handled = true;
		}
		cp.EndReadbackTransaction();
	};
	if (auto* cp = Gpu::CurrentCommandProcessor(); cp != nullptr) {
		resolve(*cp);
		return handled;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported page fault from a pre-owned resource transaction, addr=0x%016" PRIx64
		     " access=%u\n",
		     fault_vaddr, static_cast<uint32_t>(access));
	}
	EXIT_IF(m_gpu == nullptr);
	m_gpu->SendCommandSyncWithProcessor(resolve);
	return handled;
}

bool GpuResourceManager::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory invalidation from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto resolve = [this, vaddr, size](CommandProcessor& cp) {
		cp.BeginReadbackTransaction();
		{
			ResourceMutex::FaultScope fault(m_resource_mutex);
			m_buffer_cache.InvalidateMemory(vaddr, size);
			m_texture_cache.InvalidateMemory(vaddr, size);
		}
		cp.EndReadbackTransaction();
	};
	if (auto* cp = Gpu::CurrentCommandProcessor(); cp != nullptr) {
		resolve(*cp);
		return true;
	}
	if (m_resource_mutex.IsOwnedByCurrentThread()) {
		EXIT("unsupported memory invalidation from a pre-owned resource transaction, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	EXIT_IF(m_gpu == nullptr);
	m_gpu->SendCommandSyncWithProcessor(resolve);
	return true;
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

uint64_t GpuResourceManager::MappedExtent(uint64_t vaddr, uint64_t max_size) const noexcept {
	if (vaddr == 0 || max_size == 0 || vaddr >= TRACKER_ADDRESS_SIZE) {
		return 0;
	}
	if (max_size > TRACKER_ADDRESS_SIZE - vaddr) {
		max_size = TRACKER_ADDRESS_SIZE - vaddr;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.ContiguousExtent(vaddr, max_size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
	}
	m_page_manager.OnGpuMap(vaddr, size);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	Kyty::WaitWatch::Scope watch("gpu_unmap", vaddr, size); // KYTY_DIAG
	const auto             unmap = [this, vaddr, size] {
		m_buffer_cache.UnmapMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		m_page_manager.OnGpuUnmap(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	if (m_gpu == nullptr) {
		if (m_resource_mutex.IsOwnedByCurrentThread()) {
			EXIT("cannot synchronously unmap from a resource transaction\n");
		}
		unmap();
		return;
	}
	Gpu::SubmissionLock submissions(*m_gpu);
	m_gpu->SendCommandSync(unmap);
}

void GpuResourceManager::RunGarbageCollector() {
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
