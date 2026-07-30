#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/rangeSet.h"

#include <memory>
#include <span>
#include <vector>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };
enum class PageFaultPhase { Invalidate, Complete, Release };
enum class PageWatchMode { Write, ReadWrite };
enum class GpuAccess { Read, Write, ReadWrite };

using PageFaultHandler = bool (*)(void* context, PageFaultAccess access, uint64_t vaddr,
                                  uint64_t size, PageFaultPhase phase) noexcept;

class PageManager final {
public:
	class BackingWrite final {
	public:
		BackingWrite(PageManager& manager, uint64_t vaddr, uint64_t size) noexcept;
		~BackingWrite();
		KYTY_CLASS_NO_COPY(BackingWrite);

	private:
		PageManager& m_manager;
		uint64_t     m_vaddr = 0;
		uint64_t     m_size  = 0;
	};

	// Permits the inline, fault-triggered readback (buffer/texture cache) to adjust GPU page
	// watchers on the pages it downloads while the faulting thread is inside fault resolution
	// (g_in_fault_resolution set). This is exactly the watcher bookkeeping the async readback
	// workers used to perform off the resolution thread; running it inline is safe because the
	// readback is the only code executing in this window. The scope is a thread-local, nestable
	// override so that unrelated mid-resolution mutations still fail fast. Instances are cheap and
	// need no PageManager reference.
	class FaultReadbackScope final {
	public:
		FaultReadbackScope() noexcept;
		~FaultReadbackScope();
		KYTY_CLASS_NO_COPY(FaultReadbackScope);
	};
	[[nodiscard]] static bool InFaultReadback() noexcept;

	PageManager(PageFaultHandler fault_handler, void* fault_context);
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;
	[[nodiscard]] bool     IsTracked(uint64_t vaddr) const noexcept;
	[[nodiscard]] bool     IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	[[nodiscard]] bool HasGpuAccess(uint64_t vaddr, uint64_t size, GpuAccess access) const noexcept;
	// Returns the number of contiguous bytes from vaddr (capped at max_size) that have the requested
	// GPU access. Used to clamp effectively-unbounded buffer descriptors (num_records ~= 0xffffffff)
	// to the range the guest actually made GPU-visible.
	[[nodiscard]] uint64_t GpuAccessExtent(uint64_t vaddr, uint64_t max_size,
	                                       GpuAccess access) const noexcept;

	void UpdatePageWatchers(bool track, uint64_t vaddr, uint64_t size,
	                        PageWatchMode mode = PageWatchMode::Write);
	void OnGpuMap(uint64_t vaddr, uint64_t size, GpuAccess access = GpuAccess::ReadWrite);
	void OnGpuUnmap(uint64_t vaddr, uint64_t size, GpuAccess access = GpuAccess::ReadWrite);

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] std::vector<std::unique_ptr<BackingWrite>>
	ReserveBackingWrites(std::span<const RangeSet::Range> ranges);

private:
	void BeginBackingWrite(uint64_t vaddr, uint64_t size) noexcept;
	void EndBackingWrite(uint64_t vaddr, uint64_t size) noexcept;

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
