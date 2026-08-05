#ifndef KYTY_COMMON_HOST_EXCEPTION_H_
#define KYTY_COMMON_HOST_EXCEPTION_H_

#include "common/common.h"

namespace Common::HostException {

enum class ExceptionType { Unknown, AccessViolation, IllegalInstruction };

enum class AccessViolationType { Unknown, Read, Write, Execute };

struct ExceptionInfo {
	ExceptionType       type                   = ExceptionType::Unknown;
	AccessViolationType access_violation_type  = AccessViolationType::Unknown;
	uint64_t            access_violation_vaddr = 0;
	uint64_t            exception_address      = 0;
	uint64_t            rax                    = 0;
	uint64_t            rbx                    = 0;
	uint64_t            rcx                    = 0;
	uint64_t            rdx                    = 0;
	uint64_t            rsi                    = 0;
	uint64_t            rdi                    = 0;
	uint64_t            rbp                    = 0;
	uint64_t            rsp                    = 0;
	uint64_t            r8                     = 0;
	uint64_t            r9                     = 0;
	uint64_t            r10                    = 0;
	uint64_t            r11                    = 0;
	uint64_t            r12                    = 0;
	uint64_t            r13                    = 0;
	uint64_t            r14                    = 0;
	uint64_t            r15                    = 0;
	uint32_t            native_code            = 0;
	// Platform-specific mutable context, valid only for the duration of the handler call.
	void* native_context = nullptr;
};

using Handler = bool (*)(const ExceptionInfo&);

bool InstallHandler(Handler handler);

// Whether this fault arrived while another was still being resolved on the same thread.
//
// A nested fault normally terminates the process, because the fault handler is not re-entrant - it
// takes the graphics caches' locks, and those have their own recursive-acquisition guards. The one
// case where nesting is expected rather than a bug is a RenderDoc capture: RenderDoc serialises all
// mapped memory from its own thread, which walks straight into Kyty's write-tracked guest pages, and
// resolving one of those faults can fault again.
//
// With KYTY_LENIENT_HOST_FAULTS set, a nested fault is reported here instead of being fatal, so the
// handler can resolve it the only way that is safe from inside itself: make the page accessible and
// return, touching no cache. That costs the GPU-ownership bookkeeping for that page, so a capture
// taken this way can contain stale bytes for it - acceptable for a capture, which is why this is
// opt-in and not the default.
[[nodiscard]] bool IsNestedFault() noexcept;
[[nodiscard]] bool LenientNestedFaults() noexcept;

} // namespace Common::HostException

#endif /* KYTY_COMMON_HOST_EXCEPTION_H_ */
