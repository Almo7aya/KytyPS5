#include "common/waitWatch.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <set>
#include <tuple>
#include <utility>

namespace Kyty::WaitWatch {

uint64_t CurrentHostTid() {
	return GetCurrentThreadId();
}

namespace {

bool StackRangeReadable(uint64_t addr) {
	MEMORY_BASIC_INFORMATION mbi {};
	if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) {
		return false;
	}
	if (mbi.State != MEM_COMMIT) {
		return false;
	}
	const auto prot = mbi.Protect & 0xff;
	return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
	       prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
	       prot == PAGE_EXECUTE_WRITECOPY;
}

} // namespace

// Captures a host stack for a thread stuck past the dump threshold. Guest frames are FPO-less,
// so instead of walking rbp the raw stack is scanned for return addresses into the emulator
// image (0x140...) and system DLLs (0x7ff...) — symbolize emulator addresses offline (PDB).
// Captures once per unique (tid, rip) so a repeated stall dump does not spam.
void DumpThreadStackIfStuck(const ThreadWait* t, uint64_t held_ms) {
	if (held_ms < 3000) {
		return;
	}
	const auto host_tid = t->host_tid.load(std::memory_order_relaxed);
	if (host_tid == 0) {
		return;
	}
	HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
	                               THREAD_QUERY_INFORMATION,
	                           FALSE, static_cast<DWORD>(host_tid));
	if (thread == nullptr) {
		return;
	}
	if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
		CloseHandle(thread);
		return;
	}
	CONTEXT ctx {};
	ctx.ContextFlags = CONTEXT_FULL;
	if (GetThreadContext(thread, &ctx) != 0) {
		// Coarse hold-time bucket in the key so a genuine multi-second deadlock re-emits even
		// when the same (tid, rip) was already dumped during an earlier transient stall that
		// recovered. Without it the permanent dedup hid the deadlocked GPU-worker/readback
		// threads (they share ntdll's WaitOnAddress rip with prior transient stalls).
		static std::set<std::tuple<uint64_t, uint64_t, uint64_t>> captured;
		const auto key = std::make_tuple(host_tid, ctx.Rip, held_ms / 4000);
		if (captured.insert(key).second) {
			::printf("  --- host stack tid=%llu rip=0x%llx rsp=0x%llx ---\n",
			         static_cast<unsigned long long>(host_tid),
			         static_cast<unsigned long long>(ctx.Rip),
			         static_cast<unsigned long long>(ctx.Rsp));
			int found = 0;
			for (uint64_t addr = ctx.Rsp; addr < ctx.Rsp + 0x8000 && found < 40;
			     addr += sizeof(uint64_t)) {
				if (!StackRangeReadable(addr)) {
					continue;
				}
				const auto value = *reinterpret_cast<const uint64_t*>(addr);
				const bool emulator = value >= 0x140000000ull && value < 0x146000000ull;
				const bool sysdll   = value >= 0x7ff000000000ull && value < 0x800000000000ull;
				if (!emulator && !sysdll) {
					continue;
				}
				::printf("    [stk+%04llx] 0x%llx %s\n",
			                 static_cast<unsigned long long>(addr - ctx.Rsp),
			                 static_cast<unsigned long long>(value), emulator ? "emu" : "sys");
				found++;
			}
			::fflush(stdout);
		}
	}
	ResumeThread(thread);
	CloseHandle(thread);
}

} // namespace Kyty::WaitWatch

#else

namespace Kyty::WaitWatch {

uint64_t CurrentHostTid() {
	return 0;
}

void DumpThreadStackIfStuck(const ThreadWait*, uint64_t) {}

} // namespace Kyty::WaitWatch

#endif
