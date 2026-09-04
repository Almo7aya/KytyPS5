#include "graphics/host_gpu/renderer/occlusionQuery.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "kernel/memory.h"

#include <array>
#include <cstring>

namespace Libs::Graphics {

OcclusionQueryEmulator::OcclusionQueryEmulator(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {}

OcclusionQueryEmulator::~OcclusionQueryEmulator() {
	// The scheduler has been shut down by the time RenderContext destroys its members, so no
	// deferred operation can still be reading the pool.
	if (m_pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_pool, nullptr);
		m_pool = nullptr;
	}
}

void OcclusionQueryEmulator::Announce() {
	if (m_announced) {
		return;
	}
	m_announced  = true;
	m_host_reset = m_graphics.host_query_reset_enabled;
	m_precise    = m_graphics.occlusion_query_precise_enabled;
	LOGF("Occlusion queries: emulating PixelPipeStatDump with host occlusion queries "
	     "(precise=%s, host reset=%s).\n",
	     m_precise ? "yes" : "no", m_host_reset ? "yes" : "no");
}

// ---------------------------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------------------------

void OcclusionQueryEmulator::WriteGuestNow(uint64_t address, const void* data, uint32_t size) {
	// PM4-thread writes go through the guest mapping exactly as the synchronous end-of-pipe
	// writes always have, so any write-watch on the page sees them. One aligned store per value
	// keeps a concurrent guest read from ever observing a torn counter.
	if (size == sizeof(uint64_t)) {
		uint64_t value = 0;
		std::memcpy(&value, data, sizeof(value));
		*reinterpret_cast<volatile uint64_t*>(address) = value;
	} else {
		uint32_t value = 0;
		std::memcpy(&value, data, sizeof(value));
		*reinterpret_cast<volatile uint32_t*>(address) = value;
	}
}

void OcclusionQueryEmulator::WriteGuestFromDeferredOperation(uint64_t address, const void* data,
                                                             uint32_t size) {
	// Deferred operations run on the scheduler's priority thread. A store through a write-watched
	// guest page would fault there and re-enter the resource caches from the wrong thread, so go
	// through the backing store, as the GPU-readback publishers do. Addresses outside the guest
	// address space (host test memory) fall back to a direct store.
	if (!LibKernel::Memory::TryWriteBacking(address, data, size)) {
		WriteGuestNow(address, data, size);
	}
}

// ---------------------------------------------------------------------------------------------
// Dumps
// ---------------------------------------------------------------------------------------------

void OcclusionQueryEmulator::Dump(uint64_t address) {
	EXIT_IF(address == 0 || (address & 0x7u) != 0);
	Announce();

	++m_sequence;
	EvictStaleRecords(kMaxOpenRecords);

	if (auto it = m_open.find(address - sizeof(uint64_t)); it != m_open.end()) {
		auto record = std::move(it->second);
		m_open.erase(it);
		CloseRecord(address, std::move(record));
		return;
	}

	if (auto it = m_open.find(address); it != m_open.end()) {
		// A begin at an address whose end never arrived: the guest has moved on to reuse the
		// memory, so the old record can never be completed.
		DiscardRecord(std::move(it->second));
		m_open.erase(it);
	}

	OpenRecord(address);
}

void OcclusionQueryEmulator::OpenRecord(uint64_t address) {
	EvictStaleRecords(kMaxOpenRecords - 1);

	auto record      = std::make_shared<Record>();
	record->address  = address;
	record->base     = m_next_base;
	record->sequence = m_sequence;
	m_next_base += kBaseStride;

	{
		// If a result for the previous use of this query memory is still on the GPU, it must not
		// land on top of the pair that is starting now. The check-and-write in ResolveRecord
		// happens under the same lock, so either it has already written or it never will.
		std::lock_guard lock(m_closed_mutex);
		if (auto it = m_closed.find(address + sizeof(uint64_t)); it != m_closed.end()) {
			it->second->superseded.store(true, std::memory_order_release);
			m_closed.erase(it);
		}
	}

	// A begin value is final the moment it is written: the pair is measured relative to it, so
	// the guest can read it at any time.
	const uint64_t value = kValidBit | record->base;
	WriteGuestNow(address, &value, sizeof(value));

	m_open_order.emplace_back(address, record->sequence);
	m_open.emplace(address, std::move(record));
}

void OcclusionQueryEmulator::CloseRecord(uint64_t end_address, RecordPtr record) {
	const uint64_t visible = record->base + kVisibleSamples;

	if (record->slots.empty()) {
		// Nothing was drawn between the two dumps, so there is no result to wait for. The
		// hardware would report zero here; a title that brackets a draw always has one, so an
		// empty pair means the renderer dropped it somewhere this emulator does not see -- report
		// it visible rather than risk culling it.
		const uint64_t value = kValidBit | visible;
		WriteGuestNow(end_address, &value, sizeof(value));
		return;
	}

	if (!m_scheduler.Active()) {
		// No GPU work can be pending without an active scheduler.
		const uint64_t value = kValidBit | visible;
		WriteGuestNow(end_address, &value, sizeof(value));
		ReleaseSlots(record->slots);
		return;
	}

	// Provisional value: readable at once as "visible". The valid bit stays clear so a title
	// that polls for completion keeps waiting for the real result, exactly as on hardware.
	WriteGuestNow(end_address, &visible, sizeof(visible));

	{
		std::lock_guard lock(m_closed_mutex);
		if (auto it = m_closed.find(end_address); it != m_closed.end()) {
			it->second->superseded.store(true, std::memory_order_release);
		}
		m_closed[end_address] = record;
	}

	m_pending_operations.fetch_add(1, std::memory_order_acq_rel);
	m_scheduler.DeferPriorityOperation([this, record, end_address] {
		ResolveRecord(record, end_address);
		m_pending_operations.fetch_sub(1, std::memory_order_acq_rel);
	});
}

void OcclusionQueryEmulator::ResolveRecord(const RecordPtr& record, uint64_t end_address) {
	// Runs once the tick that recorded the end dump has completed, which by submission order
	// covers every command buffer the record's queries were recorded in.
	uint64_t samples  = 0;
	bool     complete = !record->tainted;
	if (complete) {
		std::lock_guard lock(m_pool_mutex);
		for (const auto slot: record->slots) {
			std::array<uint64_t, 2> data {};
			const auto              result = m_graphics.device.getQueryPoolResults(
                m_pool, slot, 1, sizeof(data), data.data(), sizeof(data),
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
			if (result != vk::Result::eSuccess || data[1] == 0) {
				complete = false;
				break;
			}
			samples += data[0];
		}
	}
	ReleaseSlots(record->slots);

	const uint64_t count = complete ? samples : kVisibleSamples;
	const uint64_t value = kValidBit | ((record->base + count) & ~kValidBit);

	std::lock_guard lock(m_closed_mutex);
	if (auto it = m_closed.find(end_address); it != m_closed.end() && it->second == record) {
		m_closed.erase(it);
	}
	if (!record->superseded.load(std::memory_order_acquire)) {
		WriteGuestFromDeferredOperation(end_address, &value, sizeof(value));
	}
}

void OcclusionQueryEmulator::DiscardRecord(RecordPtr record) {
	if (record->slots.empty()) {
		return;
	}
	if (!m_scheduler.Active()) {
		ReleaseSlots(record->slots);
		return;
	}
	// The slots may still be referenced by an in-flight command buffer; hand them back once the
	// current tick has passed.
	m_scheduler.DeferOperation([this, record = std::move(record)] { ReleaseSlots(record->slots); });
}

void OcclusionQueryEmulator::EvictStaleRecords(size_t keep_below) {
	while (!m_open_order.empty()) {
		const auto [address, sequence] = m_open_order.front();
		auto it                        = m_open.find(address);
		if (it == m_open.end() || it->second->sequence != sequence) {
			m_open_order.pop_front();
			continue;
		}
		const bool too_old  = m_sequence - sequence > kMaxRecordAge;
		const bool too_many = m_open.size() > keep_below;
		if (!too_old && !too_many) {
			break;
		}
		DiscardRecord(std::move(it->second));
		m_open.erase(it);
		m_open_order.pop_front();
	}
}

void OcclusionQueryEmulator::TaintOpenRecords() {
	for (auto& [address, record]: m_open) {
		record->tainted = true;
	}
}

// ---------------------------------------------------------------------------------------------
// Draw path
// ---------------------------------------------------------------------------------------------

OcclusionQueryEmulator::DrawScope::DrawScope(OcclusionQueryEmulator& emulator)
    : m_emulator(emulator), m_active(!emulator.m_open.empty()) {
	emulator.m_draw_emitted = false;
}

OcclusionQueryEmulator::DrawScope::~DrawScope() {
	if (m_active && !m_emulator.m_draw_emitted) {
		m_emulator.TaintOpenRecords();
	}
}

bool OcclusionQueryEmulator::EnsurePool(CommandBuffer& buffer) {
	if (m_pool != nullptr) {
		return true;
	}
	if (m_pool_failed) {
		return false;
	}

	vk::QueryPoolCreateInfo info {};
	info.sType      = vk::StructureType::eQueryPoolCreateInfo;
	info.queryType  = vk::QueryType::eOcclusion;
	info.queryCount = kPoolSize;
	if (m_graphics.device.createQueryPool(&info, nullptr, &m_pool) != vk::Result::eSuccess ||
	    m_pool == nullptr) {
		m_pool        = nullptr;
		m_pool_failed = true;
		LOGF("occlusion queries: query pool creation failed; results report visible\n");
		return false;
	}

	// Freshly created queries are in an undefined state and must be reset before their first use.
	if (m_host_reset) {
		std::lock_guard lock(m_pool_mutex);
		m_graphics.device.resetQueryPool(m_pool, 0, kPoolSize);
	} else {
		buffer.EndRendering();
		buffer.Handle().resetQueryPool(m_pool, 0, kPoolSize);
	}

	std::lock_guard lock(m_slot_mutex);
	m_slot_refs.assign(kPoolSize, 0);
	m_free_slots.resize(kPoolSize);
	for (uint32_t i = 0; i < kPoolSize; i++) {
		m_free_slots[i] = kPoolSize - 1 - i;
	}
	return true;
}

uint32_t OcclusionQueryEmulator::AcquireSlot() {
	std::lock_guard lock(m_slot_mutex);
	if (m_free_slots.empty()) {
		return kNoSlot;
	}
	const auto slot = m_free_slots.back();
	m_free_slots.pop_back();
	m_slot_refs[slot] = 0;
	return slot;
}

void OcclusionQueryEmulator::ReleaseSlots(const std::vector<uint32_t>& slots) {
	if (slots.empty()) {
		return;
	}
	std::lock_guard lock(m_slot_mutex);
	for (const auto slot: slots) {
		EXIT_IF(slot >= m_slot_refs.size() || m_slot_refs[slot] == 0);
		if (--m_slot_refs[slot] == 0) {
			m_free_slots.push_back(slot);
		}
	}
}

void OcclusionQueryEmulator::PrepareDraw(CommandBuffer& buffer) {
	m_active_slot = kNoSlot;
	if (m_open.empty()) {
		return;
	}

	bool wanted = false;
	for (const auto& [address, record]: m_open) {
		if (!record->tainted) {
			wanted = true;
			break;
		}
	}
	if (!wanted) {
		return;
	}

	if (!EnsurePool(buffer)) {
		TaintOpenRecords();
		return;
	}
	const auto slot = AcquireSlot();
	if (slot == kNoSlot) {
		TaintOpenRecords();
		return;
	}

	// A slot is only ever handed back once every command buffer that used it has completed, so
	// it can be reset here. The host reset avoids breaking the render pass instance; without the
	// feature the reset has to be recorded outside one.
	if (m_host_reset) {
		std::lock_guard lock(m_pool_mutex);
		m_graphics.device.resetQueryPool(m_pool, slot, 1);
	} else {
		buffer.EndRendering();
		buffer.Handle().resetQueryPool(m_pool, slot, 1);
	}
	m_active_slot = slot;
}

void OcclusionQueryEmulator::BeginDraw(CommandBuffer& buffer) {
	if (m_active_slot == kNoSlot) {
		return;
	}
	buffer.Handle().beginQuery(m_pool, m_active_slot,
	                           m_precise ? vk::QueryControlFlags {vk::QueryControlFlagBits::ePrecise}
	                                     : vk::QueryControlFlags {});
}

void OcclusionQueryEmulator::EndDraw(CommandBuffer& buffer) {
	m_draw_emitted = true;
	if (m_active_slot == kNoSlot) {
		return;
	}
	buffer.Handle().endQuery(m_pool, m_active_slot);

	// Like the hardware counter, one draw contributes to every query that is open around it.
	uint32_t references = 0;
	for (auto& [address, record]: m_open) {
		if (record->tainted) {
			continue;
		}
		if (record->slots.size() >= kMaxSlotsPerRecord) {
			record->tainted = true;
			continue;
		}
		record->slots.push_back(m_active_slot);
		references++;
	}

	{
		std::lock_guard lock(m_slot_mutex);
		m_slot_refs[m_active_slot] = references;
	}
	if (references == 0) {
		// Nobody kept it, but the command buffer still references it: recycle it after the tick.
		auto orphan = std::make_shared<Record>();
		orphan->slots.push_back(m_active_slot);
		{
			std::lock_guard lock(m_slot_mutex);
			m_slot_refs[m_active_slot] = 1;
		}
		DiscardRecord(std::move(orphan));
	}
	m_active_slot = kNoSlot;
}

// ---------------------------------------------------------------------------------------------
// End-of-pipe writes
// ---------------------------------------------------------------------------------------------

void OcclusionQueryEmulator::WriteEndOfPipeValue(uint64_t address, uint64_t value, uint32_t size) {
	EXIT_IF(address == 0 || (size != sizeof(uint32_t) && size != sizeof(uint64_t)));
	if (m_pending_operations.load(std::memory_order_acquire) == 0 || !m_scheduler.Active()) {
		WriteGuestNow(address, &value, size);
		return;
	}
	// Queue behind the outstanding results (and behind any label already queued, so labels never
	// overtake each other). The count includes this write, so the ordering stays sticky until the
	// queue has drained.
	m_pending_operations.fetch_add(1, std::memory_order_acq_rel);
	{
		std::lock_guard lock(m_pending_writes_mutex);
		m_pending_writes[address]++;
	}
	m_scheduler.DeferPriorityOperation([this, address, value, size] {
		WriteGuestFromDeferredOperation(address, &value, size);
		{
			std::lock_guard lock(m_pending_writes_mutex);
			if (auto it = m_pending_writes.find(address); it != m_pending_writes.end()) {
				if (--it->second == 0) {
					m_pending_writes.erase(it);
				}
			}
		}
		m_pending_operations.fetch_sub(1, std::memory_order_acq_rel);
	});
}

void OcclusionQueryEmulator::WaitForPendingEndOfPipeWrites(uint64_t address, uint64_t size) {
	if (m_pending_operations.load(std::memory_order_acquire) == 0 || !m_scheduler.Active()) {
		return;
	}
	{
		// Queued writes are 4 or 8 bytes, so a destination can start at most 7 bytes before the
		// read and still overlap it.
		std::lock_guard lock(m_pending_writes_mutex);
		bool           targeted = false;
		for (uint64_t probe = address >= 7 ? address - 7 : 0; probe < address + size; probe++) {
			if (m_pending_writes.contains(probe)) {
				targeted = true;
				break;
			}
		}
		if (!targeted) {
			return;
		}
	}
	// Everything queued so far carries a tick no later than the one this flush produces.
	m_scheduler.FlushAndWait();
	m_scheduler.WaitPriorityOperations(m_scheduler.CurrentTick() - 1);
}

} // namespace Libs::Graphics
