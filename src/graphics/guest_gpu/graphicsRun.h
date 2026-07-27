#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class CommandProcessor;

class GraphicsRunSubmissionLock final {
public:
	GraphicsRunSubmissionLock();
	~GraphicsRunSubmissionLock();
	KYTY_CLASS_NO_COPY(GraphicsRunSubmissionLock);
};

// Marks the current thread as operating inside an already-effectively-established submission pause,
// WITHOUT actually pausing the GPU worker. Nested GraphicsRunSubmissionLock acquisitions then become
// no-ops (they see a non-zero pause depth) instead of trying to (re-)pause. The async readback worker
// uses this when it services a fault requested by the command-processor (GPU worker) thread: that
// worker is blocked in the fault and can never park, but it is also not submitting any GPU work, so
// the pause its nested cache operations (e.g. BufferCache::UnmapMemory) want is already in force.
// Using a real GraphicsRunSubmissionLock there instead deadlocks (RequestPauseAndWait waits forever
// for the stuck worker to park).
class GraphicsRunAdoptedSubmissionPause final {
public:
	GraphicsRunAdoptedSubmissionPause();
	~GraphicsRunAdoptedSubmissionPause();
	KYTY_CLASS_NO_COPY(GraphicsRunAdoptedSubmissionPause);
};

void GraphicsRunInit();

void GraphicsRunSubmit(uint32_t* cmd_draw_buffer, uint32_t num_draw_dw, uint32_t* cmd_const_buffer,
                       uint32_t num_const_dw, bool trigger_agc_interrupt_on_done = false);
void GraphicsRunSubmitCompute(uint32_t queue, uint32_t* cmd_buffer, uint32_t num_dw,
                              bool trigger_agc_interrupt_on_done = false);
void GraphicsRunSubmitFlipPreparation();
void GraphicsRunWait();
void GraphicsRunDone();
int  GraphicsRunGetFrameNum();
// Diagnostic: print whether the GPU command processor is currently blocked on a WAIT_REG_MEM
// (memory-value) wait and, if so, the address/value/ref/retries it is stuck on.
void GraphicsRunDumpGpuWait();
[[nodiscard]] bool              GraphicsRunIsCommandProcessorThread() noexcept;
[[nodiscard]] CommandProcessor* GraphicsRunCurrentCommandProcessor() noexcept;
void                            GraphicsRunFinishScheduler();
[[nodiscard]] bool              GraphicsRunSubmissionLockHeld() noexcept;
[[nodiscard]] bool              GraphicsRunGpuLockHeld() noexcept;
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
