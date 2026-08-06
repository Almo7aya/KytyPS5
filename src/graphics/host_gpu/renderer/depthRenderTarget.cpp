#include "graphics/host_gpu/renderer/depthRenderTarget.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/pipeline/descriptorCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <limits>

namespace Libs::Graphics {

// Which depth compare ops the guest actually programs, split by whether depth is also written.
//
// The EQUAL-only counter this replaces returned zero, which killed the EQUAL theory but left the real operator
// unknown - so the next guess would have been just as blind. A second pass over already-written depth shows up
// here as test-enabled with write-disabled, and its operator is what a coplanar re-draw has to satisfy.
static void ReportDepthFunc(uint8_t zfunc, bool test_enable, bool write_enable) {
	static const bool enabled = std::getenv("KYTY_LOG_DEPTH_FUNC") != nullptr;
	if (!enabled) {
		return;
	}
	static std::atomic<uint64_t> counts[8][2] {};
	static std::atomic<uint64_t> total {0};
	if (!test_enable || zfunc > 7) {
		return;
	}
	counts[zfunc][write_enable ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
	const auto n = total.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n % 20000 != 0) {
		return;
	}
	static const char* names[8] = {"NEVER",   "LESS",     "EQUAL",  "LEQUAL",
	                               "GREATER", "NOTEQUAL", "GEQUAL", "ALWAYS"};
	std::fprintf(stderr, "[depth-func] total=%llu\n", static_cast<unsigned long long>(n));
	for (uint32_t i = 0; i < 8; i++) {
		const auto no_write = counts[i][0].load(std::memory_order_relaxed);
		const auto write    = counts[i][1].load(std::memory_order_relaxed);
		if (no_write != 0 || write != 0) {
			std::fprintf(stderr, "[depth-func]   %-8s write=%llu nowrite=%llu\n", names[i],
			             static_cast<unsigned long long>(write),
			             static_cast<unsigned long long>(no_write));
		}
	}
	std::fflush(stderr);
}

// How often a test-only draw's depth direction disagreed with the pass that filled the buffer. Reported at a
// low rate because the count itself is the interesting part: if this never fires, the direction mismatch seen at
// event 9697 in kyty_frame846 is not reproducing and the fix is inert.
static void ReportDepthDirectionFlip(uint64_t base, int draw_direction, int buffer_direction) {
	static std::atomic<uint64_t> count {0};
	const auto                   n = count.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n == 1 || n % 5000 == 0) {
		std::fprintf(stderr,
		             "[depth-dir] flipped=%llu base=0x%012" PRIx64 " draw_dir=%d buffer_dir=%d\n",
		             static_cast<unsigned long long>(n), base, draw_direction, buffer_direction);
		std::fflush(stderr);
	}
}

[[noreturn]] static void DepthFatal(const char* format, ...) {
	std::fputs("Depth target fatal: ", stderr);
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	EXIT("unsupported render state; details were printed above\n");
}

static vk::StencilOp ConvertStencilOp(uint8_t value) {
	switch (static_cast<Prospero::StencilOp>(value)) {
		case Prospero::StencilOp::kKeep: return vk::StencilOp::eKeep;
		case Prospero::StencilOp::kZero: return vk::StencilOp::eZero;
		case Prospero::StencilOp::kReplaceTest:
		case Prospero::StencilOp::kReplaceOp: return vk::StencilOp::eReplace;
		case Prospero::StencilOp::kAddClamp: return vk::StencilOp::eIncrementAndClamp;
		case Prospero::StencilOp::kSubClamp: return vk::StencilOp::eDecrementAndClamp;
		case Prospero::StencilOp::kInvert: return vk::StencilOp::eInvert;
		case Prospero::StencilOp::kAddWrap: return vk::StencilOp::eIncrementAndWrap;
		case Prospero::StencilOp::kSubWrap: return vk::StencilOp::eDecrementAndWrap;
		default: DepthFatal("unsupported stencil operation");
	}
}

static bool UsesStencilOpValue(uint8_t fail, uint8_t pass, uint8_t depth_fail) {
	return fail == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp) ||
	       pass == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp) ||
	       depth_fail == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp);
}

[[nodiscard]] static vk::Format ResolveHostDepthAttachmentFormat(const RenderCommandBuffer& buffer,
                                                                 const DepthFormatPolicy&   policy,
                                                                 bool     has_stencil,
                                                                 uint32_t samples) {
	auto&      graphics         = buffer.GetGraphics();
	const auto required_samples = vulkan_sample_count(samples);
	const auto supports         = [&](vk::Format format) {
		vk::ImageFormatProperties properties {};
		return format != vk::Format::eUndefined &&
		       graphics.GetImageFormatProperties(
		           format, vk::ImageType::e2D, vk::ImageTiling::eOptimal, DepthTargetImageUsage(),
		           vk::ImageCreateFlags {}, &properties) == vk::Result::eSuccess &&
		       static_cast<bool>(properties.sampleCounts & required_samples);
	};
	if (!has_stencil) {
		return supports(policy.depth_attachment_format) ? policy.depth_attachment_format
		                                                : vk::Format::eUndefined;
	}
	for (const auto format: policy.stencil_attachment_formats) {
		if (supports(format)) {
			return format;
		}
	}
	return vk::Format::eUndefined;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::ResolveRenderDepthTarget(uint64_t submit_id, RenderCommandBuffer& buffer,
                                              RenderDepthInfo& r) {
	KYTY_PROFILER_FUNCTION();
	(void)submit_id;
	const auto& hw = buffer.GetRegisters();
	const auto& z  = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	const auto& sc = hw.GetStencilControl();
	const auto& sm = hw.GetStencilMask();
	const bool  has_stencil =
	    z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid);
	const bool depth_active =
	    dc.z_enable || dc.z_write_enable || dc.depth_bounds_enable || rc.depth_clear_enable;
	const bool stencil_active = has_stencil && (dc.stencil_enable || rc.stencil_clear_enable);
	if (!depth_active && !stencil_active) {
		return;
	}
	if (!z.z_info.HasValidTextureCompatibility() ||
	    !z.stencil_info.HasValidTextureCompatibility()) {
		DepthFatal("invalid PS5 depth texture-compatibility encoding");
	}
	const bool attachment_unbound =
	    z.z_info.format == Prospero::GpuEnumValue(Prospero::DepthFormat::kInvalid) &&
	    z.stencil_info.format == Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid) &&
	    z.z_info.num_samples == 0 &&
	    z.z_info.texture_compatibility == Prospero::TextureCompatiblePlaneCompression::kDisable &&
	    !z.z_info.expclear_enabled && !z.z_info.partially_resident && z.z_info.max_mip_level == 0 &&
	    z.stencil_info.texture_compatibility == Prospero::TextureCompatibleStencil::kDisable &&
	    !z.stencil_info.expclear_enabled && !z.stencil_info.partially_resident &&
	    z.depth_view.slice_start == 0 && z.depth_view.slice_max == 0 &&
	    z.depth_view.current_mip_level == 0 && !z.depth_view.depth_write_disable &&
	    !z.depth_view.stencil_write_disable && z.depth_info.addr5_swizzle_mask == 0 &&
	    z.depth_info.array_mode == 0 && z.depth_info.pipe_config == 0 &&
	    z.depth_info.bank_width == 0 && z.depth_info.bank_height == 0 &&
	    z.depth_info.macro_tile_aspect == 0 && z.depth_info.num_banks == 0 &&
	    z.htile_surface.linear == 0 && z.htile_surface.full_cache == 0 &&
	    z.htile_surface.htile_uses_preload_win == 0 && z.htile_surface.preload == 0 &&
	    z.htile_surface.prefetch_width == 0 && z.htile_surface.prefetch_height == 0 &&
	    z.htile_surface.dst_outside_zero_to_one == 0 && z.z_read_base_addr == 0 &&
	    z.z_write_base_addr == 0 && z.stencil_read_base_addr == 0 &&
	    z.stencil_write_base_addr == 0 && z.htile_data_base_addr == 0 &&
	    // DB_DEPTH_SIZE_XY is independent state and may remain programmed after the attachment
	    // formats and addresses are unbound. A zero encoding is the valid 1x1 value, so its
	    // presence alone must not manufacture a depth attachment.
	    !z.z_info.htile_acceleration && !z.width_height_valid && !z.pitch_height_valid &&
	    z.size.x_max == 0 && z.size.y_max == 0 && z.pitch_div8_minus1 == 0 &&
	    z.height_div8_minus1 == 0 && z.slice_div64_minus1 == 0 && z.width == 0 && z.height == 0;
	if (attachment_unbound) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: ignoring enabled depth/stencil state without a bound attachment\n");
		}
		return;
	}
	const bool has_htile = z.z_info.htile_acceleration;
	const auto samples   = render_sample_count(z.z_info.num_samples);
	if (samples == 0) {
		DepthFatal("unsupported depth fragment count: %u", z.z_info.num_samples);
	}
	const bool htile_stencil_compat = depth_htile_stencil_acceleration_compatible(
	    has_stencil, has_htile, z.stencil_info.htile_stencil_disabled);
	const auto view = ResolveTargetViewInfo(z.depth_view.slice_start, z.depth_view.slice_max);
	switch (view.type) {
		case TargetViewType::Image2D:
		case TargetViewType::Image2DArray: break;
		case TargetViewType::Unsupported:
			DepthFatal("invalid depth view: base=%u last=%u", z.depth_view.slice_start,
			           z.depth_view.slice_max);
	}
	if (rc.resummarize_enable || rc.copy_centroid || rc.copy_sample != 0 ||
	    z.z_info.expclear_enabled || z.stencil_info.expclear_enabled ||
	    z.z_info.partially_resident || z.stencil_info.partially_resident ||
	    z.z_info.max_mip_level != 0 || z.depth_view.current_mip_level != 0 ||
	    z.depth_info.addr5_swizzle_mask != 0 || z.depth_info.array_mode != 0 ||
	    z.depth_info.pipe_config != 0 || z.depth_info.bank_width != 0 ||
	    z.depth_info.bank_height != 0 || z.depth_info.macro_tile_aspect != 0 ||
	    z.depth_info.num_banks != 0 || z.htile_surface.linear != 0 ||
	    z.htile_surface.full_cache != 0 || z.htile_surface.htile_uses_preload_win != 0 ||
	    z.htile_surface.preload != 0 || z.htile_surface.prefetch_width != 0 ||
	    z.htile_surface.prefetch_height != 0 || z.htile_surface.dst_outside_zero_to_one != 0 ||
	    z.z_read_base_addr == 0 || z.z_write_base_addr != z.z_read_base_addr ||
	    (z.z_read_base_addr & 0xffffu) != 0 ||
	    dc.zfunc > static_cast<uint8_t>(vk::CompareOp::eAlways)) {
		DepthFatal("unsupported depth register state");
	}
	if (has_stencil) {
		if (z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt) ||
		    !htile_stencil_compat || z.stencil_read_base_addr == 0 ||
		    z.stencil_write_base_addr != z.stencil_read_base_addr ||
		    (z.stencil_read_base_addr & 0xffffu) != 0 || z.depth_view.stencil_write_disable) {
			DepthFatal("unsupported stencil attachment state");
		}
	} else if (z.stencil_read_base_addr != 0 || z.stencil_write_base_addr != 0 ||
	           !htile_stencil_compat ||
	           z.stencil_info.texture_compatibility !=
	               Prospero::TextureCompatibleStencil::kDisable) {
		DepthFatal("stencil state without an active stencil attachment");
	}
	if (has_htile) {
		if (z.htile_data_base_addr == 0 || (z.htile_data_base_addr & 0x7fffu) != 0) {
			DepthFatal("invalid HTile metadata address");
		}
		if (z.depth_view.slice_max >= 32) {
			DepthFatal("HTile clear tracking supports at most 32 slices");
		}
	} else if (z.htile_data_base_addr != 0) {
		DepthFatal("HTile address without an enabled tile surface");
	}
	const bool size_xy_valid = z.size.valid;
	const bool wh_valid      = z.width_height_valid && z.width != 0 && z.height != 0;
	if (!size_xy_valid && !wh_valid) {
		DepthFatal("missing depth extent");
	}
	const uint32_t width  = size_xy_valid ? static_cast<uint32_t>(z.size.x_max) + 1u : z.width;
	const uint32_t height = size_xy_valid ? static_cast<uint32_t>(z.size.y_max) + 1u : z.height;
	if (width > 16384 || height > 16384 ||
	    (size_xy_valid && wh_valid && (width != z.width || height != z.height)) ||
	    (!z.pitch_height_valid &&
	     (z.pitch_div8_minus1 != 0 || z.height_div8_minus1 != 0 || z.slice_div64_minus1 != 0))) {
		DepthFatal("inconsistent depth extent or encoded layout");
	}
	const auto* policy = FindDepthFormatPolicy(z.z_info.format);
	if (policy == nullptr) {
		DepthFatal("unsupported depth/stencil format pair");
	}
	const auto ideal_format = DepthAttachmentFormat(*policy, has_stencil);
	r.format = ResolveHostDepthAttachmentFormat(buffer, *policy, has_stencil, samples);
	if (r.format == vk::Format::eUndefined) {
		DepthFatal("no host depth/stencil format supports required usage for %s",
		           VulkanToString(ideal_format).c_str());
	}
	const uint32_t guest_format = Prospero::GpuEnumValue(policy->guest_format);
	const uint32_t bytes        = policy->bytes_per_element;
	const auto     pitch        = TileGetDepthPitch(width, bytes, z.z_info.num_samples);
	if (z.pitch_height_valid && ((static_cast<uint64_t>(z.pitch_div8_minus1) + 1u) * 8u != pitch ||
	                             (static_cast<uint64_t>(z.height_div8_minus1) + 1u) * 8u !=
	                                 ((static_cast<uint64_t>(height) + 7u) & ~7ull))) {
		DepthFatal("encoded depth pitch or height mismatch");
	}
	TileSizeAlign depth_size {};
	TileSizeAlign stencil_size {};
	TileSizeAlign htile_size {};
	if (!TileGetDepthSize(width, height, 0, z.z_info.format, z.stencil_info.format, has_htile,
	                      stencil_size, htile_size, depth_size, z.z_info.num_samples) ||
	    depth_size.align != 65536 || depth_size.size == 0 ||
	    (has_stencil != (stencil_size.align == 65536 && stencil_size.size != 0)) ||
	    (has_htile != (htile_size.align == 32768 && htile_size.size != 0))) {
		DepthFatal("unsupported depth/stencil/HTile footprint");
	}
	if (z.pitch_height_valid &&
	    (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u != depth_size.size) {
		DepthFatal("depth footprint mismatch: extent=%ux%u pitch=%u expected=0x%016" PRIx64
		           " align=0x%016" PRIx64 " encoded_valid=%u encoded=0x%016" PRIx64,
		           width, height, pitch, depth_size.size, depth_size.align,
		           z.pitch_height_valid ? 1u : 0u,
		           (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u);
	}
	if (depth_size.size > UINT64_MAX / view.image_layers ||
	    stencil_size.size > UINT64_MAX / view.image_layers ||
	    htile_size.size > UINT64_MAX / view.image_layers) {
		DepthFatal("layered depth footprint overflow");
	}
	const auto depth_backing_size   = depth_size.size * view.image_layers;
	const auto stencil_backing_size = stencil_size.size * view.image_layers;
	const auto htile_backing_size   = htile_size.size * view.image_layers;
	if (depth_backing_size > TRACKER_ADDRESS_SIZE - z.z_read_base_addr ||
	    (has_stencil && stencil_backing_size > TRACKER_ADDRESS_SIZE - z.stencil_read_base_addr) ||
	    (has_htile && htile_backing_size > TRACKER_ADDRESS_SIZE - z.htile_data_base_addr)) {
		DepthFatal("layered depth backing range is invalid");
	}
	r.htile                   = has_htile;
	r.width                   = width;
	r.height                  = height;
	r.samples                 = samples;
	r.depth_buffer_size       = depth_backing_size;
	r.depth_buffer_vaddr      = z.z_read_base_addr;
	r.stencil_buffer_size     = has_stencil ? stencil_backing_size : 0;
	r.stencil_buffer_vaddr    = has_stencil ? z.stencil_read_base_addr : 0;
	r.htile_buffer_size       = has_htile ? htile_backing_size : 0;
	r.htile_buffer_vaddr      = has_htile ? z.htile_data_base_addr : 0;
	r.depth_clear_enable      = rc.depth_clear_enable;
	r.depth_meta_clear_enable = false;
	r.depth_load_clear_enable = r.depth_clear_enable;
	r.depth_clear_value       = hw.GetDepthClearValue();
	r.depth_test_enable       = dc.z_enable;
	r.depth_write_enable      = dc.z_write_enable && !z.depth_view.depth_write_disable;
	// A/B: relax an EQUAL depth test. The cast itself is right - GCN ZFUNC and VkCompareOp use the same
	// values - but EQUAL is a demand for bit-exact depth that Kyty cannot honour.
	//
	// Unreal draws the base pass with depth EQUAL against the depth its prepass already wrote, which on real
	// hardware always matches because both passes run position code that compiles identically. Kyty recompiles
	// the prepass VS and the base-pass VS into *separate* SPIR-V modules (70990 and 102910 for the character),
	// so their position arithmetic can differ by a rounding step - one may contract a multiply-add into an FMA
	// where the other does not. Wherever the results differ by even one ULP, EQUAL fails and the base pass
	// writes nothing while the prepass depth stays, which is exactly a character that occludes the road but has
	// no GBuffer normal: the legs pass and the torso does not (docs 4.1n).
	//
	// Relaxing EQUAL is safe for visibility: the prepass already stored the frontmost depth, so accepting
	// "at or in front of" cannot let hidden geometry through.
	//   KYTY_DEPTH_EQUAL_TO_GEQUAL=1 - EQUAL becomes GREATER_OR_EQUAL (the reversed-Z direction, where nearer
	//                                  is larger, which is what this title uses).
	//   KYTY_DEPTH_EQUAL_TO_LEQUAL=1 - EQUAL becomes LESS_OR_EQUAL, for the conventional direction.
	static const bool equal_to_gequal = std::getenv("KYTY_DEPTH_EQUAL_TO_GEQUAL") != nullptr;
	static const bool equal_to_lequal = std::getenv("KYTY_DEPTH_EQUAL_TO_LEQUAL") != nullptr;

	r.depth_compare_op = static_cast<vk::CompareOp>(dc.zfunc);

	// A/B: KYTY_NO_DEPTH_DIR_MATCH=1 disables this.
	//
	// A pass that only tests depth must compare in the same direction as the pass that filled the buffer.
	// Measured in _RenderDoc/kyty_frame846.rdc (docs 4.1t): the car body panel at event 9697 computes depth
	// 10/1227.76 = 0.008145 at pixel (2100,600) where the stored depth is 0.007142, written by event 90. Under
	// this title's reversed-Z (z_clip = near = 10, w = z_view, so nearer is *larger*) 0.008145 is nearer and must
	// pass - yet the draw is rejected with depth_test_failed, which only happens if it compares LESS/LEQUAL.
	// Rejecting nearer fragments is what leaves the car's body black while its wheels and roofline survive.
	//
	// So remember the direction used by the draws that write this depth buffer, and if a later test-only draw
	// arrives with the opposite direction, flip it to match. Draws that write depth are never touched, so the
	// buffer's own convention is always set by the pass that establishes it.
	static const bool no_dir_match = std::getenv("KYTY_NO_DEPTH_DIR_MATCH") != nullptr;

	const auto direction_of = [](vk::CompareOp op) -> int {
		switch (op) {
			case vk::CompareOp::eGreater:
			case vk::CompareOp::eGreaterOrEqual: return +1; // reversed Z: nearer is larger
			case vk::CompareOp::eLess:
			case vk::CompareOp::eLessOrEqual: return -1; // conventional Z: nearer is smaller
			default: return 0;                           // NEVER/EQUAL/NOTEQUAL/ALWAYS carry no direction
		}
	};

	if (!no_dir_match && r.depth_test_enable && z.z_read_base_addr != 0) {
		static Common::Mutex                 mutex;
		static std::map<uint64_t, int>       buffer_direction;
		const auto                           key       = z.z_read_base_addr;
		const auto                           direction = direction_of(r.depth_compare_op);

		Common::LockGuard lock(mutex);
		if (r.depth_write_enable) {
			if (direction != 0) {
				buffer_direction[key] = direction;
			}
		} else if (direction != 0) {
			const auto found = buffer_direction.find(key);
			if (found != buffer_direction.end() && found->second != direction) {
				// Opposite direction from the pass that filled this buffer: mirror the operator, preserving
				// whether it accepts equality.
				switch (r.depth_compare_op) {
					case vk::CompareOp::eLess: r.depth_compare_op = vk::CompareOp::eGreater; break;
					case vk::CompareOp::eLessOrEqual:
						r.depth_compare_op = vk::CompareOp::eGreaterOrEqual;
						break;
					case vk::CompareOp::eGreater: r.depth_compare_op = vk::CompareOp::eLess; break;
					case vk::CompareOp::eGreaterOrEqual:
						r.depth_compare_op = vk::CompareOp::eLessOrEqual;
						break;
					default: break;
				}
				ReportDepthDirectionFlip(key, direction, found->second);
			}
		}
	}
	if (r.depth_compare_op == vk::CompareOp::eEqual) {
		if (equal_to_gequal) {
			r.depth_compare_op = vk::CompareOp::eGreaterOrEqual;
		} else if (equal_to_lequal) {
			r.depth_compare_op = vk::CompareOp::eLessOrEqual;
		}
	}
	ReportDepthFunc(dc.zfunc, r.depth_test_enable, r.depth_write_enable);

	r.depth_bounds_test_enable = dc.depth_bounds_enable;
	r.depth_min_bounds         = hw.GetDepthBoundsMin();
	r.depth_max_bounds         = hw.GetDepthBoundsMax();

	r.stencil_clear_enable = has_stencil && rc.stencil_clear_enable;
	r.stencil_clear_value  = hw.GetStencilClearValue();
	r.stencil_test_enable  = has_stencil && dc.stencil_enable;
	if (r.stencil_test_enable) {
		if (dc.stencilfunc > static_cast<uint8_t>(vk::CompareOp::eAlways) ||
		    (dc.backface_enable &&
		     dc.stencilfunc_bf > static_cast<uint8_t>(vk::CompareOp::eAlways)) ||
		    (UsesStencilOpValue(sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail) &&
		     sm.stencil_opval != sm.stencil_testval) ||
		    (dc.backface_enable &&
		     UsesStencilOpValue(sc.stencil_fail_bf, sc.stencil_zpass_bf, sc.stencil_zfail_bf) &&
		     sm.stencil_opval_bf != sm.stencil_testval_bf)) {
			DepthFatal("unsupported stencil compare or replacement state");
		}
		r.stencil_static_front = {
		    ConvertStencilOp(sc.stencil_fail), ConvertStencilOp(sc.stencil_zpass),
		    ConvertStencilOp(sc.stencil_zfail), static_cast<vk::CompareOp>(dc.stencilfunc)};
		r.stencil_dynamic_front = {sm.stencil_mask,
		                           rc.stencil_clear_enable ? 0u : sm.stencil_writemask,
		                           sm.stencil_testval};
		if (dc.backface_enable) {
			r.stencil_static_back  = {ConvertStencilOp(sc.stencil_fail_bf),
			                          ConvertStencilOp(sc.stencil_zpass_bf),
			                          ConvertStencilOp(sc.stencil_zfail_bf),
			                          static_cast<vk::CompareOp>(dc.stencilfunc_bf)};
			r.stencil_dynamic_back = {sm.stencil_mask_bf,
			                          rc.stencil_clear_enable ? 0u : sm.stencil_writemask_bf,
			                          sm.stencil_testval_bf};
		} else {
			r.stencil_static_back  = r.stencil_static_front;
			r.stencil_dynamic_back = r.stencil_dynamic_front;
		}
	}
	r.vaddr_num = has_stencil ? 2 : 1;
	r.vaddr[0]  = r.depth_buffer_vaddr;
	r.size[0]   = r.depth_buffer_size;
	if (has_stencil) {
		r.vaddr[1] = r.stencil_buffer_vaddr;
		r.size[1]  = r.stencil_buffer_size;
	}
	TextureCache::ImageDesc desc {};
	desc.type                 = TextureCache::BindingType::DepthTarget;
	desc.info.data            = {r.depth_buffer_vaddr, r.depth_buffer_size};
	desc.info.stencil         = {r.stencil_buffer_vaddr, r.stencil_buffer_size};
	desc.info.pixel_format    = r.format;
	desc.info.guest_format    = guest_format;
	desc.info.type            = Prospero::ImageType::kColor2D;
	desc.info.extent          = {width, height, 1};
	desc.info.resources       = {1, view.image_layers};
	desc.info.pitch           = pitch;
	desc.info.bytes_per_block = bytes;
	desc.info.samples         = samples;
	desc.info.tile_mode       = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
	desc.info.mip_layout[0]   = {0, r.depth_buffer_size, pitch, height};
	desc.info.metadata.range  = {r.htile_buffer_vaddr, r.htile_buffer_size};
	desc.info.metadata.kind   = has_htile ? ImageMetadataKind::Htile : ImageMetadataKind::None;
	desc.info.metadata.stencil_compressed =
	    has_stencil && has_htile && !z.stencil_info.htile_stencil_disabled;
	desc.view_info.format = r.format;
	desc.view_info.type =
	    view.layer_count == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray;
	desc.view_info.aspect      = ImageViewOps::DepthAspectMask(r.format);
	desc.view_info.base_level  = 0;
	desc.view_info.level_count = 1;
	desc.view_info.base_layer  = view.base_layer;
	desc.view_info.layer_count = view.layer_count;
	desc.view_info.usage       = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	r.desc                     = std::move(desc);
	auto& cache                = m_context.GetTextureCache();
	r.image_id                 = cache.FindImage(r.desc);
	r.image_view               = nullptr;
	BindRenderTarget(r.image_id);
}

vk::ImageAspectFlags RenderDepthInfo::AttachmentWriteAspects() const {
	if (format == vk::Format::eUndefined) {
		return {};
	}

	const auto           available = ImageViewOps::DepthAspectMask(format);
	vk::ImageAspectFlags writes {};
	if ((available & vk::ImageAspectFlagBits::eDepth) &&
	    (depth_load_clear_enable || (depth_test_enable && depth_write_enable))) {
		writes |= vk::ImageAspectFlagBits::eDepth;
	}
	if (!(available & vk::ImageAspectFlagBits::eStencil)) {
		return writes;
	}

	const auto face_writes = [&](const PipelineStencilStaticState&  state,
	                             const PipelineStencilDynamicState& dynamic) {
		if (dynamic.writeMask == 0) {
			return false;
		}
		bool can_pass = state.compareOp != vk::CompareOp::eNever;
		bool can_fail = state.compareOp != vk::CompareOp::eAlways;
		if (dynamic.compareMask == 0) {
			switch (state.compareOp) {
				case vk::CompareOp::eEqual:
				case vk::CompareOp::eLessOrEqual:
				case vk::CompareOp::eGreaterOrEqual:
				case vk::CompareOp::eAlways:
					can_pass = true;
					can_fail = false;
					break;
				case vk::CompareOp::eNever:
				case vk::CompareOp::eLess:
				case vk::CompareOp::eGreater:
				case vk::CompareOp::eNotEqual:
					can_pass = false;
					can_fail = true;
					break;
				default: break;
			}
		}
		const bool depth_pass = !depth_test_enable || depth_compare_op != vk::CompareOp::eNever;
		const bool depth_fail = depth_test_enable && depth_compare_op != vk::CompareOp::eAlways;
		return (can_fail && state.failOp != vk::StencilOp::eKeep) ||
		       (can_pass && depth_pass && state.passOp != vk::StencilOp::eKeep) ||
		       (can_pass && depth_fail && state.depthFailOp != vk::StencilOp::eKeep);
	};
	if (stencil_clear_enable ||
	    (stencil_test_enable && (face_writes(stencil_static_front, stencil_dynamic_front) ||
	                             face_writes(stencil_static_back, stencil_dynamic_back)))) {
		writes |= vk::ImageAspectFlagBits::eStencil;
	}
	return writes;
}

} // namespace Libs::Graphics
