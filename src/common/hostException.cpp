#include "common/hostException.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
#elif defined(__APPLE__)
#include <csignal>
#include <sys/ucontext.h>
#else
#include <csignal>
#include <initializer_list>
#include <ucontext.h> // IWYU pragma: keep
#include <unistd.h>
#endif

// IWYU pragma: no_include <errhandlingapi.h>
// IWYU pragma: no_include <excpt.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <wtypes.h>

namespace Common::HostException {

#if !defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};
static thread_local bool    g_in_exception_filter = false;

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);

[[noreturn]] static void FailFast(const char* reason) noexcept {
	std::fputs("HostException fail-fast: ", stderr);
	std::fputs(reason != nullptr ? reason : "unspecified", stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXCEPTION_NONCONTINUABLE_EXCEPTION));
#endif
	std::_Exit(321);
}

class FilterScope final {
public:
	FilterScope() noexcept {
		if (g_in_exception_filter) {
			FailFast("nested exception while resolving a host fault");
		}
		g_in_exception_filter = true;
	}

	~FilterScope() { g_in_exception_filter = false; }

	KYTY_CLASS_NO_COPY(FilterScope);
};

static Handler LoadInstalledHandler() noexcept {
	if (g_install_state.load(std::memory_order_acquire) == 0) {
		FailFast("host exception handler is not installed");
	}

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler == nullptr) {
		FailFast("host exception callback is null");
	}
	return handler;
}
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

// Resume trampoline for guest-mode continuations. The Windows 11 24H2 guarded
// context-restore path validates the continuation RSP against the thread's
// stack, so a guest RSP fail-fasts before the resume. The exception filter
// instead gives the dispatcher a host-stack RSP and this trampoline address
// (both valid), then the trampoline swaps in the guest RSP and jumps to the
// guest RIP without touching any general-purpose register:
//   +0x00: xchg rsp, [rip+0x09]   ; 48 87 25 09 00 00 00
//   +0x07: nop                    ; 90
//   +0x08: jmp  [rip+0x0a]        ; ff 25 0a 00 00 00
//   +0x10: rsp_slot (guest RSP)
//   +0x18: rip_slot (guest RIP)
struct Trampoline {
	alignas(8) uint8_t code[16] {};
	uint64_t            rsp_slot = 0;
	uint64_t            rip_slot = 0;
};

static_assert(offsetof(Trampoline, rsp_slot) == 0x10);
static_assert(offsetof(Trampoline, rip_slot) == 0x18);

static constexpr uint32_t kTrampolineCount = 64;
static std::mutex         g_trampoline_mutex;
static Trampoline*        g_trampolines[kTrampolineCount] {};
static uint32_t           g_trampoline_threads[kTrampolineCount] {};

static Trampoline* AcquireTrampoline() {
	const auto thread_id = static_cast<uint32_t>(GetCurrentThreadId());
	std::lock_guard lock(g_trampoline_mutex);

	uint32_t free_slot = kTrampolineCount;
	for (uint32_t i = 0; i < kTrampolineCount; i++) {
		if (g_trampolines[i] != nullptr) {
			if (g_trampoline_threads[i] == thread_id) {
				return g_trampolines[i];
			}
		} else if (free_slot == kTrampolineCount) {
			free_slot = i;
		}
	}
	if (free_slot == kTrampolineCount) {
		return nullptr;
	}

	auto* tramp = static_cast<Trampoline*>(VirtualAlloc(nullptr, sizeof(Trampoline),
	                                                    MEM_COMMIT | MEM_RESERVE,
	                                                    PAGE_EXECUTE_READWRITE));
	if (tramp == nullptr) {
		return nullptr;
	}

	const uint8_t bytes[16] = {0x48, 0x87, 0x25, 0x09, 0x00, 0x00, 0x00, 0x90,
	                           0xFF, 0x25, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00};
	for (uint32_t i = 0; i < sizeof(bytes); i++) {
		tramp->code[i] = bytes[i];
	}
	FlushInstructionCache(GetCurrentProcess(), tramp->code, sizeof(tramp->code));

	g_trampolines[free_slot]        = tramp;
	g_trampoline_threads[free_slot] = thread_id;
	return tramp;
}

static LONG WINAPI ExceptionFilter(PEXCEPTION_POINTERS exception) {
	FilterScope filter_scope;

	auto* exception_record = exception->ExceptionRecord;

	if (exception_record->ExceptionCode == DBG_PRINTEXCEPTION_C ||
	    exception_record->ExceptionCode == DBG_PRINTEXCEPTION_WIDE_C) {
		return EXCEPTION_CONTINUE_SEARCH;
	}

	if (exception_record->ExceptionCode == 0x406D1388) {
		// Set a thread name.
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	ExceptionInfo info {};
	info.exception_address = reinterpret_cast<uint64_t>(exception_record->ExceptionAddress);
	info.native_code       = exception_record->ExceptionCode;
	info.native_context    = exception->ContextRecord;

	if (exception_record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
		info.type = ExceptionType::AccessViolation;
		switch (exception_record->ExceptionInformation[0]) {
			case 0: info.access_violation_type = AccessViolationType::Read; break;
			case 1: info.access_violation_type = AccessViolationType::Write; break;
			case 8: info.access_violation_type = AccessViolationType::Execute; break;
			default: info.access_violation_type = AccessViolationType::Unknown; break;
		}
		info.access_violation_vaddr = exception_record->ExceptionInformation[1];
	} else if (exception_record->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		printf("Unhandled win exception: code=0x%08" PRIx32 ", addr=0x%016" PRIx64
		       ", rip=0x%016" PRIx64 ", rsp=0x%016" PRIx64 ", rbp=0x%016" PRIx64 "\n",
		       static_cast<uint32_t>(exception_record->ExceptionCode),
		       reinterpret_cast<uint64_t>(exception_record->ExceptionAddress),
		       exception->ContextRecord->Rip, exception->ContextRecord->Rsp,
		       exception->ContextRecord->Rbp);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	info.rax = exception->ContextRecord->Rax;
	info.rbx = exception->ContextRecord->Rbx;
	info.rcx = exception->ContextRecord->Rcx;
	info.rdx = exception->ContextRecord->Rdx;
	info.rsi = exception->ContextRecord->Rsi;
	info.rdi = exception->ContextRecord->Rdi;
	info.rbp = exception->ContextRecord->Rbp;
	info.rsp = exception->ContextRecord->Rsp;
	info.r8  = exception->ContextRecord->R8;
	info.r9  = exception->ContextRecord->R9;
	info.r10 = exception->ContextRecord->R10;
	info.r11 = exception->ContextRecord->R11;
	info.r12 = exception->ContextRecord->R12;
	info.r13 = exception->ContextRecord->R13;
	info.r14 = exception->ContextRecord->R14;
	info.r15 = exception->ContextRecord->R15;

	const auto handler = LoadInstalledHandler();

	if (!handler(info)) {
		std::fprintf(stderr,
		             "HOST EXCEPTION **unresolved**: type=%s addr=0x%016" PRIx64
		             " fault_ip=0x%016" PRIx64 "\n",
		             info.type == ExceptionType::IllegalInstruction ? "illegal_instruction"
		                                                            : "access_violation",
		             info.access_violation_vaddr, info.exception_address);
		std::fprintf(stderr,
		             "  regs: rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64
		             " rdx=%016" PRIx64 " rsi=%016" PRIx64 " rdi=%016" PRIx64 " rbp=%016" PRIx64
		             " rsp=%016" PRIx64 " r8=%016" PRIx64 " r9=%016" PRIx64 " r10=%016" PRIx64
		             " r11=%016" PRIx64 " r12=%016" PRIx64 " r13=%016" PRIx64 " r14=%016" PRIx64
		             " r15=%016" PRIx64 "\n",
		             info.rax, info.rbx, info.rcx, info.rdx, info.rsi, info.rdi, info.rbp,
		             info.rsp, info.r8, info.r9, info.r10, info.r11, info.r12, info.r13,
		             info.r14, info.r15);
		std::fflush(stderr);
		return EXCEPTION_CONTINUE_SEARCH;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Windows 11 24H2 dispatches EXCEPTION_CONTINUE_EXECUTION through
	// ntdll!RtlGuardRestoreContext, which fail-fasts (0xc0000409,
	// FAST_FAIL_INVALID_SET_OF_CONTEXT) when the continuation RSP is not
	// within the current thread's stack. Guest code resumes with a guest-mode
	// RSP, so the resume is redirected through a per-thread trampoline: the
	// dispatcher is given a host-stack RSP (valid) and the trampoline address
	// (module code, valid), and the trampoline itself swaps in the guest RSP
	// and jumps to the guest RIP using only the RSP register, preserving all
	// guest general-purpose registers.
	if (auto* trampoline = AcquireTrampoline(); trampoline != nullptr) {
		auto* const context = exception->ContextRecord;
		trampoline->rsp_slot = context->Rsp;
		trampoline->rip_slot = context->Rip;
		context->Rip         = reinterpret_cast<uintptr_t>(trampoline->code);
		context->Rsp         = reinterpret_cast<uintptr_t>(&context);
		// Preserve the exception's original ContextFlags. In particular, clearing
		// CONTEXT_XSTATE loses the upper halves of AVX registers when a guest
		// instruction faults and is retried through this trampoline.
		return EXCEPTION_CONTINUE_EXECUTION;
	}
#endif

	return EXCEPTION_CONTINUE_EXECUTION;
}

#elif defined(__APPLE__)

static std::atomic<Handler> g_handler {nullptr};
static std::atomic_uint32_t g_install_state {0};
static thread_local bool    g_in_exception_filter = false;

static_assert(decltype(g_handler)::is_always_lock_free);
static_assert(decltype(g_install_state)::is_always_lock_free);

[[noreturn]] static void FailFast(const char* reason) noexcept {
	std::fputs("HostException fail-fast: ", stderr);
	std::fputs(reason != nullptr ? reason : "unspecified", stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	std::_Exit(321);
}

// Translate the x86-64 page-fault error code (mcontext __es.__err) into an access type.
// bit 1 (0x2) = write, bit 4 (0x10) = instruction fetch, otherwise a read.
static AccessViolationType DecodeAccess(uint64_t err) {
	if ((err & 0x10u) != 0) {
		return AccessViolationType::Execute;
	}
	if ((err & 0x2u) != 0) {
		return AccessViolationType::Write;
	}
	return AccessViolationType::Read;
}

// POSIX signal handler that mirrors the Windows vectored handler: build an ExceptionInfo
// from the mcontext and dispatch. A resolved fault (handler returns true) simply returns,
// re-executing the faulting instruction against the now-fixed protection. An unresolved
// fault restores the default disposition so the retry terminates the process.
static void SignalHandler(int sig, siginfo_t* si, void* uctx) {
	if (g_in_exception_filter) {
		FailFast("nested exception while resolving a host fault");
	}
	g_in_exception_filter = true;

	auto*       uc = static_cast<ucontext_t*>(uctx);
	const auto* mc = uc->uc_mcontext;
	const auto& ss = mc->__ss;

	ExceptionInfo info {};
	info.exception_address = ss.__rip;
	info.native_code       = static_cast<uint32_t>(si->si_code);
	info.native_context    = uctx;

	if (sig == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		info.type                   = ExceptionType::AccessViolation;
		info.access_violation_type  = DecodeAccess(mc->__es.__err);
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(si->si_addr);
	}

	info.rax = ss.__rax;
	info.rbx = ss.__rbx;
	info.rcx = ss.__rcx;
	info.rdx = ss.__rdx;
	info.rsi = ss.__rsi;
	info.rdi = ss.__rdi;
	info.rbp = ss.__rbp;
	info.rsp = ss.__rsp;
	info.r8  = ss.__r8;
	info.r9  = ss.__r9;
	info.r10 = ss.__r10;
	info.r11 = ss.__r11;
	info.r12 = ss.__r12;
	info.r13 = ss.__r13;
	info.r14 = ss.__r14;
	info.r15 = ss.__r15;

	const auto handler = g_handler.load(std::memory_order_acquire);
	if (handler == nullptr) {
		FailFast("host exception callback is null");
	}

	const bool resolved   = handler(info);
	g_in_exception_filter = false;

	if (resolved) {
		return; // retry the faulting instruction against the fixed mapping
	}

	// Unresolved: restore the default action so the re-executed instruction terminates.
	struct sigaction dfl {};
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	sigaction(sig, &dfl, nullptr);
}

#else

// x86-64 page-fault error bits.
constexpr uint64_t PAGE_FAULT_ERROR_WRITE       = 0x02;
constexpr uint64_t PAGE_FAULT_ERROR_INSTRUCTION = 0x10;

// Let the kernel handle an unresolved fault on retry.
static void ChainToDefault(int signal_number) noexcept {
	struct sigaction restore {};
	restore.sa_handler = SIG_DFL;
	sigemptyset(&restore.sa_mask);
	restore.sa_flags = 0;
	::sigaction(signal_number, &restore, nullptr);
}

static void SignalHandler(int signal_number, siginfo_t* signal_info, void* native_context) {
	FilterScope filter_scope;

	auto* context = static_cast<ucontext_t*>(native_context);
	auto* gregs   = context->uc_mcontext.gregs;

	ExceptionInfo info {};
	info.exception_address = static_cast<uint64_t>(gregs[REG_RIP]);
	info.native_code       = static_cast<uint32_t>(signal_number);
	info.native_context    = context;

	if (signal_number == SIGSEGV || signal_number == SIGBUS) {
		info.type             = ExceptionType::AccessViolation;
		const auto error_code = static_cast<uint64_t>(gregs[REG_ERR]);
		if ((error_code & PAGE_FAULT_ERROR_INSTRUCTION) != 0) {
			info.access_violation_type = AccessViolationType::Execute;
		} else if ((error_code & PAGE_FAULT_ERROR_WRITE) != 0) {
			info.access_violation_type = AccessViolationType::Write;
		} else {
			info.access_violation_type = AccessViolationType::Read;
		}
		info.access_violation_vaddr = reinterpret_cast<uint64_t>(signal_info->si_addr);
	} else if (signal_number == SIGILL) {
		info.type = ExceptionType::IllegalInstruction;
	} else {
		ChainToDefault(signal_number);
		return;
	}

	info.rax = static_cast<uint64_t>(gregs[REG_RAX]);
	info.rbx = static_cast<uint64_t>(gregs[REG_RBX]);
	info.rcx = static_cast<uint64_t>(gregs[REG_RCX]);
	info.rdx = static_cast<uint64_t>(gregs[REG_RDX]);
	info.rsi = static_cast<uint64_t>(gregs[REG_RSI]);
	info.rdi = static_cast<uint64_t>(gregs[REG_RDI]);
	info.rbp = static_cast<uint64_t>(gregs[REG_RBP]);
	info.rsp = static_cast<uint64_t>(gregs[REG_RSP]);
	info.r8  = static_cast<uint64_t>(gregs[REG_R8]);
	info.r9  = static_cast<uint64_t>(gregs[REG_R9]);
	info.r10 = static_cast<uint64_t>(gregs[REG_R10]);
	info.r11 = static_cast<uint64_t>(gregs[REG_R11]);
	info.r12 = static_cast<uint64_t>(gregs[REG_R12]);
	info.r13 = static_cast<uint64_t>(gregs[REG_R13]);
	info.r14 = static_cast<uint64_t>(gregs[REG_R14]);
	info.r15 = static_cast<uint64_t>(gregs[REG_R15]);

	const auto handler = LoadInstalledHandler();

	if (handler(info)) {
		return;
	}

	ChainToDefault(signal_number);
}

#endif

bool InstallHandler(Handler handler) {
	if (handler == nullptr) {
		return false;
	}

	uint32_t expected_state = 0;
	if (!g_install_state.compare_exchange_strong(expected_state, 1, std::memory_order_acq_rel)) {
		return expected_state == 2 && g_handler.load(std::memory_order_acquire) == handler;
	}

	g_handler.store(handler, std::memory_order_release);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// Windows 11 24H2 dispatches a vectored-handler continuation through
	// ntdll!RtlGuardRestoreContext, which validates the continuation RSP by
	// calling RtlGuardIsValidStackPointer.  That validator reads the current
	// thread's TEB stack bounds, and guest threads (which run with a guest
	// RSP and a guest-mode TEB) never pass it, so the dispatcher fail-fasts
	// (0xc0000409, FAST_FAIL_INVALID_SET_OF_CONTEXT) before resuming guest
	// code.  The validation block is skipped when the process shadow-stack
	// policy byte at LdrSystemDllInitBlock+0x9c has bit 0 set; set it so the
	// dispatcher restores the continuation context as-is (the guest resume
	// itself is handled by the trampoline below).
	HMODULE ntdll_module = GetModuleHandleW(L"ntdll.dll");
	uint8_t* guard_policy =
	    (ntdll_module != nullptr)
	        ? reinterpret_cast<uint8_t*>(GetProcAddress(ntdll_module, "LdrSystemDllInitBlock"))
	        : nullptr;
	if (guard_policy != nullptr) {
		DWORD old_protect = 0;
		if (VirtualProtect(guard_policy + 0x9c, 1, PAGE_READWRITE, &old_protect) != 0) {
			guard_policy[0x9c] |= 0x01u;
			VirtualProtect(guard_policy + 0x9c, 1, old_protect, &old_protect);
			printf("host_exception: guarded-restore workaround active (policy byte=0x%02x)\n",
			       guard_policy[0x9c]);
		} else {
			printf("host_exception: guarded-restore RSP validation patch failed (error=%lu)\n",
			       static_cast<unsigned long>(GetLastError()));
		}
	}

	if (AddVectoredExceptionHandler(1, ExceptionFilter) == nullptr) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("AddVectoredExceptionHandler() failed\n");
		return false;
	}
#elif defined(__APPLE__)
	struct sigaction sa {};
	sa.sa_sigaction = SignalHandler;
	sa.sa_flags     = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	// The guest signal-dispatch path (KernelRaiseException) interrupts threads with
	// SIGUSR1; block it while a fault is being resolved so a stop-the-world request
	// cannot preempt the handler between the protection fix and the retry.
	sigaddset(&sa.sa_mask, SIGUSR1);

	// macOS raises SIGBUS for protection faults on some paths and SIGSEGV on others;
	// SIGILL covers instructions the host cannot execute (routed to the x64 emulator).
	bool ok = sigaction(SIGSEGV, &sa, nullptr) == 0 && sigaction(SIGBUS, &sa, nullptr) == 0 &&
	          sigaction(SIGILL, &sa, nullptr) == 0;
	if (!ok) {
		g_handler.store(nullptr, std::memory_order_release);
		g_install_state.store(0, std::memory_order_release);
		printf("sigaction() failed to install the host fault handler\n");
		return false;
	}
#else
	struct sigaction action {};
	action.sa_sigaction = SignalHandler;
	sigemptyset(&action.sa_mask);
	// Fault resolution needs the normal thread stack.
	action.sa_flags = SA_SIGINFO | SA_RESTART;

	for (const int signal_number: {SIGSEGV, SIGBUS, SIGILL}) {
		if (::sigaction(signal_number, &action, nullptr) != 0) {
			g_handler.store(nullptr, std::memory_order_release);
			g_install_state.store(0, std::memory_order_release);
			printf("sigaction(%d) failed\n", signal_number);
			return false;
		}
	}
#endif

	g_install_state.store(2, std::memory_order_release);
	return true;
}

} // namespace Common::HostException
