#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERY_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandBuffer;
class CommandScheduler;

// Emulates the depth-block Z-pass counters that PIXEL_PIPE_STAT_DUMP samples into guest memory,
// backed by host occlusion queries.
//
// What the guest expects. The hardware keeps a free-running per-DB count of samples that passed
// the depth test. A dump writes the current count, with bit 63 set as a "valid" flag, for every DB
// at `address + db * 16`. A title issues one dump right before its occlusion-test draw and a second
// one 8 bytes above it right after, then reads `end - begin` on the CPU a frame later and drops the
// primitive from the next frame's draw list when the difference is zero. Only the first DB slot is
// written here; that is the behaviour the always-visible stand-in already validated against the
// titles seen so far.
//
// How it is emulated. A dump whose address is not 8 bytes above an outstanding begin dump *is* a
// begin: it opens a record and is published at once with a fresh synthetic base value B. Every
// draw executed while records are open is bracketed by a Vulkan occlusion query, and the query
// slot is attached to each open record. The dump 8 bytes above a begin closes its record. It is
// published twice: immediately as B + kVisibleSamples with the valid bit clear, and once the GPU
// has produced the result as B + samples with the valid bit set. The guest's difference is thus
// never negative, reads as "visible" until the real result exists, and becomes the true sample
// count once it does. Anything that makes the count untrustworthy -- a draw the renderer dropped,
// a query slot that could not be allocated, an unavailable result -- keeps the "visible" value,
// since over-draw is a frame-rate cost while a wrongly culled primitive is a visible defect.
//
// Why end-of-pipe writes route through here as well. Kyty writes end-of-pipe labels at PM4
// time, before the GPU has executed anything. A title that waits on a frame fence before reading
// its query results would therefore read the provisional values every time and never cull. While
// results are outstanding, label writes are queued behind them on the scheduler's priority queue,
// which preserves the order the guest relies on: results land before the fence that covers them.
// Titles that never issue a dump never take that path.
//
// Threading. Dump(), the draw hooks and WriteEndOfPipeValue() run on the PM4 thread. Results are
// read and published from the scheduler's priority thread once the GPU tick has passed; the state
// those two sides share is limited to the query-slot free list, the closed-record table and the
// pending-operation counter, each of which is guarded here.
class OcclusionQueryEmulator {
public:
	OcclusionQueryEmulator(GraphicContext& graphics, CommandScheduler& scheduler);
	~OcclusionQueryEmulator();
	KYTY_CLASS_NO_COPY(OcclusionQueryEmulator);

	// PIXEL_PIPE_STAT_DUMP with an 8-byte aligned, non-null destination.
	void Dump(uint64_t address);

	// Draw path, in this order: PrepareDraw before the render pass instance is (re)opened for the
	// draw, BeginDraw once the pipeline is bound, EndDraw immediately after the draw command.
	void PrepareDraw(CommandBuffer& buffer);
	void BeginDraw(CommandBuffer& buffer);
	void EndDraw(CommandBuffer& buffer);

	// Brackets one guest draw from the point the renderer accepts it. A draw that the renderer
	// then fails to emit would still have been counted by the hardware, so every record it would
	// have contributed to is reported visible instead of being left at zero.
	class DrawScope {
	public:
		explicit DrawScope(OcclusionQueryEmulator& emulator);
		~DrawScope();
		KYTY_CLASS_NO_COPY(DrawScope);

		// The packet turned out not to be a draw (a metadata clear, for instance).
		void Dismiss() noexcept { m_active = false; }

	private:
		OcclusionQueryEmulator& m_emulator;
		bool                    m_active;
	};

	// Guest-visible end-of-pipe value writes: labels, fences, flip markers. Written immediately
	// unless occlusion results are outstanding, in which case the write is ordered behind them.
	void WriteEndOfPipeValue(uint64_t address, uint64_t value, uint32_t size);

	// For PM4 packets that read guest memory an end-of-pipe write may still be heading for.
	void WaitForPendingEndOfPipeWrites();

private:
	static constexpr uint32_t kNoSlot            = UINT32_MAX;
	static constexpr uint32_t kPoolSize          = 8192;
	static constexpr uint32_t kMaxOpenRecords    = 64;
	static constexpr uint32_t kMaxSlotsPerRecord = 64;
	static constexpr uint64_t kMaxRecordAge      = 8192; // in dumps
	static constexpr uint64_t kValidBit          = 1ull << 63u;
	static constexpr uint64_t kVisibleSamples    = 1ull << 24u;
	static constexpr uint64_t kBaseStride        = 1ull << 26u;

	struct Record {
		uint64_t              address  = 0; // begin dump destination
		uint64_t              base     = 0; // value published for the begin dump
		uint64_t              sequence = 0; // dump sequence number when opened
		std::vector<uint32_t> slots;
		bool                  tainted = false;
		// Set when the guest reuses the query memory before the result was published, so the
		// stale result must not overwrite the newer pair.
		std::atomic<bool> superseded {false};
	};
	using RecordPtr = std::shared_ptr<Record>;

	void Announce();
	void OpenRecord(uint64_t address);
	void CloseRecord(uint64_t end_address, RecordPtr record);
	void ResolveRecord(const RecordPtr& record, uint64_t end_address);
	void DiscardRecord(RecordPtr record);
	void EvictStaleRecords(size_t keep_below);
	void TaintOpenRecords();
	bool EnsurePool(CommandBuffer& buffer);
	[[nodiscard]] uint32_t AcquireSlot();
	void                   ReleaseSlots(const std::vector<uint32_t>& slots);

	static void WriteGuestNow(uint64_t address, const void* data, uint32_t size);
	static void WriteGuestFromDeferredOperation(uint64_t address, const void* data, uint32_t size);

	GraphicContext&   m_graphics;
	CommandScheduler& m_scheduler;
	bool              m_host_reset  = false;
	bool              m_precise     = false;
	bool              m_announced   = false;
	bool              m_pool_failed = false;

	// PM4-thread state.
	std::unordered_map<uint64_t, RecordPtr>   m_open;
	std::deque<std::pair<uint64_t, uint64_t>> m_open_order; // (address, sequence)
	uint64_t                                  m_next_base    = kBaseStride;
	uint64_t                                  m_sequence     = 0;
	uint32_t                                  m_active_slot  = kNoSlot;
	bool                                      m_draw_emitted = false;

	// Closed records whose result is still on the GPU, keyed by end-dump address.
	std::mutex                              m_closed_mutex;
	std::unordered_map<uint64_t, RecordPtr> m_closed;

	// Host resets and result reads on the pool come from different threads.
	std::mutex    m_pool_mutex;
	vk::QueryPool m_pool = nullptr;

	std::mutex            m_slot_mutex;
	std::vector<uint32_t> m_free_slots;
	std::vector<uint32_t> m_slot_refs;

	// Priority operations queued by this object that have not run yet: result publications and
	// the end-of-pipe writes ordered behind them.
	std::atomic<uint32_t> m_pending_operations {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_OCCLUSIONQUERY_H_
