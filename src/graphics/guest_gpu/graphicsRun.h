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
