#include "graphics/host_gpu/memoryTracker.h"

#include "common/assert.h"

namespace Libs::Graphics {

#if defined(KYTY_MEMORY_TRACKER_TESTS)
namespace {
std::atomic<MemoryTracker::UnmapContentionHook> g_unmap_contention_hook {nullptr};
}

void MemoryTracker::SetUnmapContentionHook(UnmapContentionHook hook) noexcept {
	g_unmap_contention_hook.store(hook, std::memory_order_release);
}
#endif

static_assert(std::atomic<void*>::is_always_lock_free);

MemoryTracker::MemoryTracker(PageManager& page_manager, PageWatchMode gpu_watch_mode)
    : m_page_manager(page_manager), m_gpu_watch_mode(gpu_watch_mode) {
	switch (m_gpu_watch_mode) {
		case PageWatchMode::Write:
		case PageWatchMode::ReadWrite: break;
		default: EXIT("unsupported memory tracker GPU page-watch mode\n");
	}
	m_regions = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
	for (size_t i = 0; i < REGION_COUNT; i++) {
		m_regions[i].store(nullptr, std::memory_order_relaxed);
	}
}

MemoryTracker::~MemoryTracker() = default;

void MemoryTracker::ValidateRange(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0 || vaddr >= TRACKER_ADDRESS_SIZE ||
	    size > TRACKER_ADDRESS_SIZE - vaddr) {
		EXIT("invalid memory tracker range\n");
	}
}

RegionManager* MemoryTracker::GetOrCreateRegion(uint64_t index) {
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	std::lock_guard lock(m_region_mutex);
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	auto  manager = std::make_unique<RegionManager>(m_page_manager, index * TRACKER_REGION_SIZE);
	auto* ptr     = manager.get();
	m_region_storage.push_back(std::move(manager));
	m_regions[index].store(ptr, std::memory_order_release);
	return ptr;
}

bool MemoryTracker::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::lock_guard access(m_access_mutex);
	RequireMapped(vaddr, size);
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Cpu>(offset, bytes);
	});
}

bool MemoryTracker::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::lock_guard access(m_access_mutex);
	RequireMapped(vaddr, size);
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::lock_guard access(m_access_mutex);
	RequireMapped(vaddr, size);
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		const auto       changed =
		    manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyProtection(changed, false);
	});
}

void MemoryTracker::MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::lock_guard access(m_access_mutex);
	RequireMapped(vaddr, size);
	Iterate<true>(vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		const auto       changed =
		    manager->ChangeState<DirtySource::Gpu, true>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyGpuProtection(changed, true, m_gpu_watch_mode);
	});
}

void MemoryTracker::UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::lock_guard access(m_access_mutex);
	RequireMapped(vaddr, size);
	Iterate<true>(vaddr, size, [this](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		if (!manager->IsFullyModified<DirtySource::Gpu>(offset, bytes)) {
			EXIT("cannot clear partially GPU-dirty tracking range\n");
		}
		const auto changed =
		    manager->ChangeState<DirtySource::Gpu, false>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyGpuProtection(changed, false, m_gpu_watch_mode);
	});
}

void MemoryTracker::UntrackMemoryLocked(uint64_t vaddr, uint64_t size) {
	RequireMapped(vaddr, size);

	std::vector<RegionManager*> managers;
	managers.reserve((vaddr % TRACKER_REGION_SIZE + size + TRACKER_REGION_SIZE - 1) /
	                 TRACKER_REGION_SIZE);
	Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
		managers.push_back(manager);
	});

	std::vector<std::unique_lock<TrackingSpinLock>> locks;
	locks.reserve(managers.size());
	for (auto* manager: managers) {
		locks.emplace_back(manager->lock);
	}
	if (Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		    return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	    })) {
		EXIT("cannot untrack GPU-dirty memory\n");
	}
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		const auto changed =
		    manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
		manager->ApplyProtection(changed, false);
		manager->Untrack(manager->GetCpuAddr() + offset, bytes);
	});
	locks.clear();
}

void MemoryTracker::UntrackMemory(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::lock_guard access(m_access_mutex);
	UntrackMemoryLocked(vaddr, size);
}

void MemoryTracker::UnmapMemory(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback(this, vaddr);
	std::unique_lock access(m_access_mutex, std::try_to_lock);
	if (!access.owns_lock()) {
#if defined(KYTY_MEMORY_TRACKER_TESTS)
		if (const auto hook = g_unmap_contention_hook.load(std::memory_order_acquire);
		    hook != nullptr) {
			hook();
		}
#endif
		access.lock();
	}
	UntrackMemoryLocked(vaddr, size);
	m_page_manager.OnGpuUnmap(vaddr, size);
}

bool MemoryTracker::InvalidateRegion(uint64_t vaddr, uint64_t size, PageFaultPhase phase) noexcept {
	switch (phase) {
		case PageFaultPhase::Release: return true;
		case PageFaultPhase::Invalidate: {
			const auto action = BeginCpuFault(vaddr, size);
			switch (action) {
				case CpuFaultAction::Untracked: return false;
				case CpuFaultAction::Continue: return true;
				case CpuFaultAction::Download:
					EXIT("generic region invalidation cannot download GPU-dirty memory\n");
			}
		}
		case PageFaultPhase::Complete:
			return CompleteCpuFault(vaddr, size, PageFaultAccess::Write, false);
	}
	EXIT("unsupported region invalidation phase\n");
}

bool MemoryTracker::InvalidateVirtualGpuWrite(PageFaultAccess access, uint64_t vaddr, uint64_t size,
                                              PageFaultPhase phase) noexcept {
	switch (phase) {
		case PageFaultPhase::Release: return true;
		case PageFaultPhase::Invalidate: {
			const bool gpu_modified = Iterate<false>(
			    vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				    std::scoped_lock lock(manager->lock);
				    return manager->IsModified<DirtySource::Gpu>(offset, bytes);
			    });
			if (!gpu_modified) {
				return false;
			}
			const auto action = BeginCpuFault(vaddr, size);
			if (access != PageFaultAccess::Write || action != CpuFaultAction::Download) {
				EXIT("virtual GPU write fault requires write access to GPU-dirty memory\n");
			}
			return true;
		}
		case PageFaultPhase::Complete: {
			if (access != PageFaultAccess::Write) {
				EXIT("virtual GPU write completion requires write access\n");
			}
			bool completed = false;
			Iterate<false>(
			    vaddr, size, [&completed](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				    std::scoped_lock lock(manager->lock);
				    if (completed) {
					    EXIT("virtual GPU write fault spans multiple tracked regions\n");
				    }
				    completed =
				        manager->CompleteVirtualGpuWrite(manager->GetCpuAddr() + offset, bytes);
			    });
			return completed;
		}
	}
	EXIT("unsupported virtual GPU write invalidation phase\n");
}

CpuFaultAction MemoryTracker::BeginCpuFault(uint64_t vaddr, uint64_t size,
                                            PageFaultAccess access) noexcept {
	if (s_upload_owner == this) {
		// A page fault on the thread that is inside a ForEachUploadRange callback on THIS
		// tracker. Regions whose lock is not held by this thread are handled normally (their
		// spinlock is free, so there is no deadlock to guard against). Regions already held by
		// this thread are mid state-transition in the outer upload: the lock owner is the
		// thread itself, so the region cannot change under it, and the outer operation rewrites
		// the region state (ChangeState + ApplyGpuProtection) when it completes. Such regions
		// are passed through WITHOUT touching their state — the page manager makes the faulted
		// page accessible after completion — and the passthrough is recorded so the matching
		// CompleteCpuFault succeeds without state changes.
		CpuFaultAction action = CpuFaultAction::Untracked;
		Iterate<false>(
		    vaddr, size, [this, &action, access](RegionManager* manager, uint64_t offset,
		                                         uint64_t bytes) {
			    if (manager->lock.OwnedByCurrentThread()) {
				    if (action != CpuFaultAction::Untracked || s_nested_fault_manager != nullptr) {
					    EXIT("nested CPU fault spans multiple upload-owned regions\n");
				    }
				    s_nested_fault_manager = manager;
				    s_nested_fault_vaddr   = manager->GetCpuAddr() + offset;
				    s_nested_fault_size    = bytes;
				    action                 = CpuFaultAction::Continue;
				    return;
			    }
			    std::scoped_lock lock(manager->lock);
			    if (action != CpuFaultAction::Untracked) {
				    EXIT("CPU fault spans multiple tracked regions\n");
			    }
			    action = manager->BeginCpuFault(manager->GetCpuAddr() + offset, bytes, access);
		    });
		return action;
	}
	CheckNotInUploadCallback(this, vaddr);
	CpuFaultAction action = CpuFaultAction::Untracked;
	Iterate<false>(
	    vaddr, size, [&action, access](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		    std::scoped_lock lock(manager->lock);
		    if (action != CpuFaultAction::Untracked) {
			    EXIT("CPU fault spans multiple tracked regions\n");
		    }
		    action = manager->BeginCpuFault(manager->GetCpuAddr() + offset, bytes, access);
	    });
	return action;
}

bool MemoryTracker::CompleteCpuFault(uint64_t vaddr, uint64_t size, PageFaultAccess access,
                                     bool downloaded) noexcept {
	if (s_upload_owner == this) {
		if (s_nested_fault_manager != nullptr) {
			if (vaddr != s_nested_fault_vaddr || size != s_nested_fault_size) {
				EXIT("nested CPU fault completion does not match its passthrough\n");
			}
			s_nested_fault_manager = nullptr;
			s_nested_fault_vaddr   = 0;
			s_nested_fault_size    = 0;
			return true;
		}
		bool found = false;
		Iterate<false>(
		    vaddr, size,
		    [&found, access, downloaded](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			    std::scoped_lock lock(manager->lock);
			    if (found) {
				    EXIT("CPU fault completion spans multiple tracked regions\n");
			    }
			    found = manager->CompleteCpuFault(manager->GetCpuAddr() + offset, bytes, access,
			                                      downloaded);
		    });
		return found;
	}
	CheckNotInUploadCallback(this, vaddr);
	bool found = false;
	Iterate<false>(
	    vaddr, size,
	    [&found, access, downloaded](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		    std::scoped_lock lock(manager->lock);
		    if (found) {
			    EXIT("CPU fault completion spans multiple tracked regions\n");
		    }
		    found = manager->CompleteCpuFault(manager->GetCpuAddr() + offset, bytes, access,
		                                      downloaded);
	    });
	return found;
}

} // namespace Libs::Graphics
