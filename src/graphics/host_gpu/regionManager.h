#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <xxhash.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		if (m_owner.load(std::memory_order_relaxed) == thread) {
			EXIT("recursive region tracking lock\n");
		}
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			if (m_owner.load(std::memory_order_relaxed) == thread) {
				EXIT("recursive region tracking lock while contended\n");
			}
			std::atomic_signal_fence(std::memory_order_seq_cst);
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		if (m_owner.load(std::memory_order_relaxed) != CurrentThread()) {
			EXIT("region tracking lock released by non-owner\n");
		}
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

class RegionManager final {
public:
	RegionManager(PageManager& page_manager, uint64_t cpu_addr)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		const auto& bits        = GetBits<source>();
		return RegionBits(bits, start, end).Any();
	}

	// How an upload treats pages that the CPU rewrites every frame ("hot"). A hot page is left
	// dirty and writable so it never faults again; instead it is re-uploaded at most once per
	// submission. A GPU write to the page ends that regime because GPU-dirty and CPU-dirty are
	// mutually exclusive states.
	enum class HotPolicy { Preserve, ClearAll, ClearAndUnhot };

	// Global submission generation; advanced once per completed GPU submission.
	static void AdvanceGeneration() noexcept {
		s_generation.fetch_add(1, std::memory_order_relaxed);
	}

	// Forget hot state for pages leaving the cache or joining a new host buffer. The stored
	// content hashes must go too: a new buffer needs every dirty page uploaded regardless.
	void ClearHot(uint64_t vaddr, uint64_t size) {
		if (!m_hot_any && !m_hot_hash) {
			return;
		}
		const auto [start, end] = GetPageRange(vaddr, size);
		m_hot.UnsetRange(start, end);
		m_hot_uploaded.UnsetRange(start, end);
		m_faulted_recently.UnsetRange(start, end);
		if (m_hot_hash) {
			std::fill(m_hot_hash->begin() + static_cast<std::ptrdiff_t>(start),
			          m_hot_hash->begin() + static_cast<std::ptrdiff_t>(end), 0);
		}
		m_hot_any = m_hot.Any();
	}

	template <DirtySource source, bool enable, bool from_fault = false>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
			if constexpr (from_fault) {
				NoteCpuFault(start, end);
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			UpdateCpuProtection<!enable>();
		} else {
			UpdateGpuProtection<enable>();
		}
	}

	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func,
	                          HotPolicy policy = HotPolicy::ClearAll) {
		const auto [start, end] = GetPageRange(vaddr, size);
		RegionBits mask(GetBits<source>(), start, end);
		if constexpr (source == DirtySource::Cpu && clear) {
			if (policy == HotPolicy::Preserve && m_hot_any) {
				RefreshHotUploads();
				const RegionBits hot_in_range(m_hot, start, end);
				if (hot_in_range.Any()) {
					// Hot pages already uploaded this submission drop out of the upload set; the
					// rest stay dirty (and writable) but are recorded as uploaded.
					mask &= ~(hot_in_range & m_hot_uploaded);
					m_hot_uploaded |= hot_in_range;
					// A hot page is often bound in every submission but rewritten only once per
					// frame. Hash its contents and upload only when they actually changed.
					const auto candidates = mask & hot_in_range;
					if (candidates.Any()) {
						if (!m_hot_hash) {
							m_hot_hash =
							    std::make_unique<std::array<uint64_t, TRACKER_REGION_PAGES>>();
							m_hot_hash->fill(0);
						}
						for (const auto [first, last]: candidates) {
							for (auto page = first; page < last; page++) {
								const auto hash = XXH3_64bits(
								    reinterpret_cast<const void*>(m_cpu_addr +
								                                  page * TRACKER_PAGE_SIZE),
								    TRACKER_PAGE_SIZE);
								if (hash == (*m_hot_hash)[page]) {
									mask.Unset(page);
								} else {
									(*m_hot_hash)[page] = hash;
								}
							}
						}
					}
					auto to_clear = mask & ~m_hot;
					m_cpu_dirty &= ~to_clear;
					UpdateCpuProtection<true>();
					ForEachRange(mask, std::forward<Func>(func));
					return;
				}
			}
			GetBits<source>().UnsetRange(start, end);
			if (policy == HotPolicy::ClearAndUnhot && m_hot_any) {
				m_hot.UnsetRange(start, end);
				m_hot_uploaded.UnsetRange(start, end);
				m_hot_any = m_hot.Any();
			}
			UpdateCpuProtection<true>();
			ForEachRange(mask, std::forward<Func>(func));
			return;
		}
		if constexpr (clear) {
			GetBits<source>().UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Gpu && clear) {
			UpdateGpuProtection<false>();
		}
		ForEachRange(mask, std::forward<Func>(func));
	}

	TrackingSpinLock lock;

private:
	template <bool track>
	void UpdateCpuProtection() {
		auto mask  = m_cpu_dirty ^ m_writable;
		m_writable = m_cpu_dirty;
		if (mask.None()) {
			return;
		}
		m_page_manager.UpdatePageWatchersForRegion<track>(m_cpu_addr, mask);
	}

	template <bool track>
	void UpdateGpuProtection() {
		auto readable = ~m_gpu_dirty;
		auto mask     = readable ^ m_readable;
		m_readable    = readable;
		if (mask.None()) {
			return;
		}
		if constexpr (track) {
			m_page_manager.UpdatePageWatchersForRegion<true, true>(m_cpu_addr, mask);
		} else {
			m_page_manager.UpdatePageWatchersForRegion<false, true>(m_cpu_addr, mask);
		}
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	template <typename Func>
	void ForEachRange(const RegionBits& bits, Func&& func) const {
		for (const auto [start, end]: bits) {
			func(m_cpu_addr + start * TRACKER_PAGE_SIZE, (end - start) * TRACKER_PAGE_SIZE);
		}
	}

	// A page that faults twice within FaultWindow submissions is hot. The window is a coarse
	// generation stamp: the "faulted recently" set is wiped whenever it is older than the window.
	// Wider windows (128) turned once-per-frame rewrites hot too, but hashing those pages every
	// submission cost more than their faults did.
	static constexpr uint64_t FaultWindow = 16;

	void NoteCpuFault(size_t start, size_t end) {
		if (!Config::HotPageTrackingEnabled()) {
			return;
		}
		const auto generation = s_generation.load(std::memory_order_relaxed);
		if (generation - m_fault_generation >= FaultWindow) {
			m_faulted_recently.Clear();
			m_fault_generation = generation;
		}
		const RegionBits range_recent(m_faulted_recently, start, end);
		if (range_recent.Any()) {
			const auto newly_hot = range_recent & ~m_hot;
			if (newly_hot.Any()) {
				m_hot |= newly_hot;
				m_hot_any = true;
			}
		}
		m_faulted_recently.SetRange(start, end);
	}

	void RefreshHotUploads() {
		const auto generation = s_generation.load(std::memory_order_relaxed);
		if (generation != m_upload_generation) {
			m_hot_uploaded.Clear();
			m_upload_generation = generation;
		}
	}

	inline static std::atomic_uint64_t s_generation {0};

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
	RegionBits   m_hot;
	RegionBits   m_hot_uploaded;
	RegionBits   m_faulted_recently;
	std::unique_ptr<std::array<uint64_t, TRACKER_REGION_PAGES>> m_hot_hash;
	uint64_t     m_fault_generation  = 0;
	uint64_t     m_upload_generation = 0;
	bool         m_hot_any           = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
