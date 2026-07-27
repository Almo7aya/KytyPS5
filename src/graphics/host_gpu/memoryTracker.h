#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_

#include "common/assert.h"
#include "common/waitWatch.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace Libs::Graphics {

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager&  page_manager,
	                       PageWatchMode gpu_watch_mode = PageWatchMode::ReadWrite);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UntrackMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] CpuFaultAction
	                   BeginCpuFault(uint64_t vaddr, uint64_t size,
	                                 PageFaultAccess access = PageFaultAccess::Write) noexcept;
	[[nodiscard]] bool CompleteCpuFault(uint64_t vaddr, uint64_t size, PageFaultAccess access,
	                                    bool downloaded) noexcept;
	[[nodiscard]] bool InvalidateRegion(uint64_t vaddr, uint64_t size,
	                                    PageFaultPhase phase) noexcept;
	[[nodiscard]] bool InvalidateVirtualGpuWrite(PageFaultAccess access, uint64_t vaddr,
	                                             uint64_t size, PageFaultPhase phase) noexcept;

	template <bool clear, typename Preflight, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Preflight&& preflight, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Preflight&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		CheckNotInUploadCallback(this, vaddr);
		Kyty::WaitWatch::Scope w("mt_dl", vaddr, size); // KYTY_DIAG
		std::lock_guard access(m_access_mutex);
		RequireMapped(vaddr, size);
		std::vector<RegionManager*> managers;
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
			managers.push_back(manager);
		});
		std::vector<std::unique_lock<TrackingSpinLock>> locks;
		locks.reserve(managers.size());
		for (auto* manager: managers) {
			locks.emplace_back(manager->lock);
		}
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const auto address = manager->GetCpuAddr() + offset;
			if (manager->HasPendingFault(address, bytes)) {
				EXIT("GPU download synchronization raced a pending CPU fault\n");
			}
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(address, bytes,
			                                                                preflight);
		});
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(
			    manager->GetCpuAddr() + offset, bytes, func);
		});
		if constexpr (clear) {
			Iterate<false>(vaddr, size,
			               [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               const auto address = manager->GetCpuAddr() + offset;
				               const auto changed =
				                   manager->template ForEachModifiedRange<DirtySource::Gpu, true>(
				                       address, bytes, [](uint64_t, uint64_t) noexcept {});
				               manager->ApplyGpuProtection(changed, false, m_gpu_watch_mode);
			               });
		}
	}

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		ForEachDownloadRange<clear>(
		    vaddr, size, [](uint64_t, uint64_t) noexcept {}, std::forward<Func>(func));
	}

#if defined(KYTY_MEMORY_TRACKER_TESTS)
	using UnmapContentionHook = void (*)() noexcept;
	static void SetUnmapContentionHook(UnmapContentionHook hook) noexcept;
#endif

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		CheckNotInUploadCallback(this, vaddr);
		Kyty::WaitWatch::Scope w("mt_ul", vaddr, size); // KYTY_DIAG
		std::unique_lock access(m_access_mutex);
		RequireMapped(vaddr, size);
		Iterate<true>(vaddr, size, [](RegionManager*, uint64_t, uint64_t) {});
		s_upload_owner = this;
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			manager->lock.lock();
			manager->Track(manager->GetCpuAddr() + offset, bytes);
			manager->ForEachModifiedRange<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset,
			                                                      bytes, range_func);
			if (!is_written) {
				manager->lock.unlock();
			}
		});
		upload_func();
		if (is_written) {
			Iterate<false>(
			    vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				    const auto changed = manager->template ChangeState<DirtySource::Gpu, true>(
				        manager->GetCpuAddr() + offset, bytes);
				    manager->ApplyGpuProtection(changed, true, m_gpu_watch_mode);
				    manager->lock.unlock();
			    });
		}
		s_upload_owner = nullptr;
	}

private:
	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	inline static thread_local const MemoryTracker* s_upload_owner = nullptr;
	// Nested page-fault passthrough (see BeginCpuFault): the region+page a fault was allowed
	// through without touching region state, so the matching CompleteCpuFault can succeed.
	inline static thread_local const RegionManager* s_nested_fault_manager = nullptr;
	inline static thread_local uint64_t           s_nested_fault_vaddr   = 0;
	inline static thread_local uint64_t           s_nested_fault_size    = 0;

	static void CheckNotInUploadCallback(const MemoryTracker* self, uint64_t vaddr = 0) noexcept {
		if (s_upload_owner != nullptr) {
			// KYTY_DIAG: characterize the re-entry before dying (same-instance re-entry can
			// deadlock on a held region spinlock, cross-instance is a false positive).
			const auto& w = Kyty::WaitWatch::Self();
			std::printf("KYTY_DIAG tracker re-entry: self=%p owner=%p same=%d vaddr=0x%llx "
			            "thread=%s state=%s arg0=0x%llx\n",
			            static_cast<const void*>(self), static_cast<const void*>(s_upload_owner),
			            s_upload_owner == self ? 1 : 0, static_cast<unsigned long long>(vaddr),
			            w.name.load(std::memory_order_relaxed),
			            w.kind.load(std::memory_order_relaxed),
			            static_cast<unsigned long long>(w.arg0.load(std::memory_order_relaxed)));
			std::fflush(stdout);
			EXIT("memory tracker re-entered from upload callback\n");
		}
	}

	template <bool create, typename Func>
	bool Iterate(uint64_t vaddr, uint64_t size, Func&& func) {
		ValidateRange(vaddr, size);
		using Result = std::invoke_result_t<Func, RegionManager*, uint64_t, uint64_t>;
		constexpr bool returns_bool = std::is_same_v<Result, bool>;
		uint64_t       remaining    = size;
		uint64_t       index        = vaddr / TRACKER_REGION_SIZE;
		uint64_t       offset       = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr && create) {
				manager = GetOrCreateRegion(index);
			}
			if (manager != nullptr) {
				if constexpr (returns_bool) {
					if (func(manager, offset, bytes)) {
						return true;
					}
				} else {
					func(manager, offset, bytes);
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return false;
	}

	static void ValidateRange(uint64_t vaddr, uint64_t size);
	void        UntrackMemoryLocked(uint64_t vaddr, uint64_t size);
	void        RequireMapped(uint64_t vaddr, uint64_t size) const {
		ValidateRange(vaddr, size);
		if (!m_page_manager.IsMapped(vaddr, size)) {
			EXIT("memory tracker range is not mapped\n");
		}
	}
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	std::mutex                                     m_access_mutex;
	PageManager&                                   m_page_manager;
	PageWatchMode                                  m_gpu_watch_mode = PageWatchMode::ReadWrite;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
