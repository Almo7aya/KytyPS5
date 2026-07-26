#ifndef EMULATOR_SRC_COMMON_WAITWATCH_H_
#define EMULATOR_SRC_COMMON_WAITWATCH_H_

// Lightweight guest-thread wait tracker + stall watchdog support.
//
// Each thread records what blocking primitive it is currently parked on (an atomic, plain writes,
// no locks on the hot path). A background watchdog samples the render frame counter and, when it
// stops advancing, dumps every registered thread's current wait so we can tell a genuine deadlock
// (all threads parked on the same primitive) from a thrash (threads still churning through waits)
// from a crash. Diagnostic only.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace Kyty::WaitWatch {

// Lock-free unique-vs-total guest-fault measurement (Phase 1 diagnosis). Tells whether the fault
// storm is re-faults of the same pages (eager re-protection => coalescing helps) or unique pages
// (genuine streaming volume). One atomic bit test-set + two counters per fault.
namespace FaultStats {
inline constexpr uint64_t kBase  = 0x2000000000ULL;      // guest GPU address window base
inline constexpr uint64_t kSpan  = 0x400000000ULL;       // 16 GiB covers observed 0x20xx-0x23xx
inline constexpr uint64_t kPages = kSpan / 0x1000ULL;    // 4,194,304 pages
inline constexpr uint64_t kWords = kPages / 64ULL;

inline std::atomic<uint64_t> total {0};
inline std::atomic<uint64_t> unique {0};

inline std::atomic<uint64_t>* Bits() {
	static std::atomic<uint64_t>* bits = new std::atomic<uint64_t>[kWords](); // value-init to 0
	return bits;
}

inline void Record(uint64_t vaddr) {
	total.fetch_add(1, std::memory_order_relaxed);
	if (vaddr < kBase) {
		return;
	}
	const uint64_t page = (vaddr - kBase) / 0x1000ULL;
	if (page >= kPages) {
		return;
	}
	const uint64_t mask = 1ULL << (page % 64ULL);
	const uint64_t prev = Bits()[page / 64ULL].fetch_or(mask, std::memory_order_relaxed);
	if ((prev & mask) == 0) {
		unique.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace FaultStats

inline uint64_t NowMs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

struct ThreadWait {
	std::atomic<uint64_t>    activity {0}; // bumped on every wait enter+exit; static => parked
	std::atomic<const char*> kind {"start"};
	std::atomic<uint64_t>    arg0 {0};
	std::atomic<uint64_t>    arg1 {0};
	std::atomic<uint64_t>    start_ms {0};
	std::atomic<const char*> name {"?"};
	std::atomic<int32_t>     tid {0};
	std::atomic<uint64_t>    host_tid {0};
};

inline std::mutex& RegMutex() {
	static std::mutex m;
	return m;
}
inline std::vector<ThreadWait*>& Registry() {
	static std::vector<ThreadWait*> v;
	return v;
}

inline ThreadWait& Self() {
	thread_local ThreadWait* self = [] {
		auto* t = new ThreadWait();
		std::scoped_lock lk(RegMutex());
		Registry().push_back(t);
		return t;
	}();
	return *self;
}

inline void SetThreadName(const char* n, int32_t tid, uint64_t host_tid) {
	auto& s = Self();
	s.name.store(n, std::memory_order_relaxed);
	s.tid.store(tid, std::memory_order_relaxed);
	s.host_tid.store(host_tid, std::memory_order_relaxed);
}

inline void Begin(const char* kind, uint64_t a0, uint64_t a1) {
	auto& s = Self();
	s.kind.store(kind, std::memory_order_relaxed);
	s.arg0.store(a0, std::memory_order_relaxed);
	s.arg1.store(a1, std::memory_order_relaxed);
	s.start_ms.store(NowMs(), std::memory_order_relaxed);
	s.activity.fetch_add(1, std::memory_order_relaxed);
}

inline void End() {
	auto& s = Self();
	s.kind.store("running", std::memory_order_relaxed);
	s.activity.fetch_add(1, std::memory_order_relaxed);
}

// RAII marker placed just before a blocking call. Saves and restores the previous state so nested
// waits (e.g. a GPU pause that internally waits for idle) report the innermost primitive and unwind
// back to the outer one on exit.
struct Scope {
	const char* prev_kind;
	uint64_t    prev_a0;
	uint64_t    prev_a1;
	uint64_t    prev_start;

	explicit Scope(const char* kind, uint64_t a0 = 0, uint64_t a1 = 0) {
		auto& s    = Self();
		prev_kind  = s.kind.load(std::memory_order_relaxed);
		prev_a0    = s.arg0.load(std::memory_order_relaxed);
		prev_a1    = s.arg1.load(std::memory_order_relaxed);
		prev_start = s.start_ms.load(std::memory_order_relaxed);
		Begin(kind, a0, a1);
	}
	~Scope() {
		auto& s = Self();
		s.kind.store(prev_kind, std::memory_order_relaxed);
		s.arg0.store(prev_a0, std::memory_order_relaxed);
		s.arg1.store(prev_a1, std::memory_order_relaxed);
		s.start_ms.store(prev_start, std::memory_order_relaxed);
		s.activity.fetch_add(1, std::memory_order_relaxed);
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
};

inline void Dump(const char* reason) {
	std::scoped_lock lk(RegMutex());
	const uint64_t   now = NowMs();
	::printf("=== WAITWATCH (%s) threads=%zu ===\n", reason, Registry().size());
	for (auto* t: Registry()) {
		const uint64_t start = t->start_ms.load(std::memory_order_relaxed);
		const uint64_t held  = (start != 0 && now >= start) ? (now - start) : 0;
		::printf("  tid=%d host=%llu name=%s state=%s arg0=0x%llx arg1=0x%llx held_ms=%llu act=%llu\n",
		         t->tid.load(std::memory_order_relaxed),
		         static_cast<unsigned long long>(t->host_tid.load(std::memory_order_relaxed)),
		         t->name.load(std::memory_order_relaxed),
		         t->kind.load(std::memory_order_relaxed),
		         static_cast<unsigned long long>(t->arg0.load(std::memory_order_relaxed)),
		         static_cast<unsigned long long>(t->arg1.load(std::memory_order_relaxed)),
		         static_cast<unsigned long long>(held),
		         static_cast<unsigned long long>(t->activity.load(std::memory_order_relaxed)));
	}
	::fflush(stdout);
}

} // namespace Kyty::WaitWatch

#endif // EMULATOR_SRC_COMMON_WAITWATCH_H_
