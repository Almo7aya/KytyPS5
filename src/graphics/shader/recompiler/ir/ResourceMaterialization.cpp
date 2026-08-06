#include "graphics/shader/recompiler/ir/ResourceMaterialization.h"

#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/ir/ScalarProvenance.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

// Last descriptor that decoded to a plausible base, per (shader, buffer slot).
//
// Buffer 2 of the character shaders is the skinning bone-matrix buffer: stride 16, 262 records (~87 bone
// matrices at 3 rows each), read by fourteen consecutive buffer_load_format_xyzw on s[12:15]. Zeroing it binds
// the 16-byte null buffer, so every bone matrix reads zero and skinned positions collapse - while the vertex
// attributes, which come from the attrib table at s30-31 and are correctly rewritten, stay exact. That is the
// wrong-w / correct-parameters split measured in §4.1o, and why only skinned meshes break.
//
// The guest does supply a valid descriptor for it (observed base 0x1440f46fc0, stride 16, records 0x106) and
// the user-data write path is verified correct - every write arrives via the direct SET_SH_REG route with
// reg_num == write_num - so an implausible value means those slots currently hold another draw's data. Reusing
// the last good descriptor keeps skinning working: at worst a pose one frame stale, rather than a mesh
// collapsed to the origin.
//
// A/B: KYTY_NO_DESCRIPTOR_FALLBACK=1 goes back to zeroing (the null binding).
std::mutex                                               g_last_good_mutex;
std::map<std::pair<uint64_t, uint32_t>, DescriptorValue>  g_last_good_descriptors;

bool DisableDescriptorFallback() {
	static const bool disabled = std::getenv("KYTY_NO_DESCRIPTOR_FALLBACK") != nullptr;
	return disabled;
}

// Per (shader, buffer slot) good/bad tallies.
//
// The aggregate rate cannot answer whether this is the flicker's cause: "good" sums every buffer of every
// shader, so 1000 bad against millions of good says nothing about one slot of one shader. It also explains why
// the last-good fallback changed nothing - if this slot is bad on *every* materialisation there is never a good
// value to fall back to, and the fallback is a no-op by construction.
//
// A slot that is bad nearly always means the guest does not put a descriptor there for this shader at all, and
// the bone V# must arrive by another route - the buffer table at s28-29 is the obvious candidate. A slot that
// is bad only occasionally means the value is real but read at the wrong moment.
void TallyDescriptor(uint64_t shader_hash, uint32_t slot, bool ok) {
	struct Tally {
		uint64_t good = 0;
		uint64_t bad  = 0;
	};
	static std::mutex                                        mutex;
	static std::map<std::pair<uint64_t, uint32_t>, Tally>    tallies;
	static uint64_t                                          reports = 0;

	std::scoped_lock lock(mutex);
	auto&            entry = tallies[std::make_pair(shader_hash, slot)];
	if (ok) {
		entry.good++;
	} else {
		entry.bad++;
	}
	if (!ok && ++reports % 200 == 0) {
		std::fprintf(stderr, "[desc-tally] ---- after %llu bad ----\n",
		             static_cast<unsigned long long>(reports));
		for (const auto& [key, value]: tallies) {
			if (value.bad != 0) {
				std::fprintf(stderr,
				             "[desc-tally] shader=0x%016" PRIx64 " slot=%" PRIu32 " good=%llu bad=%llu\n",
				             key.first, key.second, static_cast<unsigned long long>(value.good),
				             static_cast<unsigned long long>(value.bad));
			}
		}
		std::fflush(stderr);
	}
}

Decoder::ImageDimension DescriptorDimension(const DescriptorValue&  descriptor,
                                            Decoder::ImageDimension requested) {
	const bool is_array = requested == Decoder::ImageDimension::Dim1DArray ||
	                      requested == Decoder::ImageDimension::Dim2DArray ||
	                      requested == Decoder::ImageDimension::Dim2DMsaaArray;
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor1D: return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor1DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim1DArray;
			}
			return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor2DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DArray;
			}
			return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaaArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DMsaaArray;
			}
			return Decoder::ImageDimension::Dim2DMsaa;
		case Prospero::ImageType::kColor2D: return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2DMsaa;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor) {
	const auto type   = static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (type < Prospero::ImageType::kColor1D || format == Prospero::BufferFormat::kInvalid) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 && max_mip == fragments;
	}
	return true;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

uint64_t AddressSpecialization(const AddressResource&           resource,
                               const ResourceSnapshot::Address& snapshot) {
	return resource.kind == ResourceKind::Flat || resource.source == ScalarProvenance::Unknown
	           ? snapshot.binding_base
	           : snapshot.guest_base - snapshot.binding_base;
}

} // namespace

bool ValidateResourceSnapshot(const Program& program, const ResourceSnapshot& snapshot,
                              std::string* error) {
	if (!program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "shader resources were not tracked";
		}
		return false;
	}
	if (snapshot.buffers.size() != program.info.buffers.size() ||
	    snapshot.images.size() != program.info.images.size() ||
	    snapshot.samplers.size() != program.info.samplers.size() ||
	    snapshot.addresses.size() != program.info.addresses.size()) {
		if (error != nullptr) {
			*error = "resource snapshot does not match dense shader topology";
		}
		return false;
	}
	if (snapshot.flattened_srt.size() != program.srt.reads.size()) {
		if (error != nullptr) {
			*error = "flattened SRT snapshot does not match the shader plan";
		}
		return false;
	}
	if (program.binding_layout_complete) {
		for (const auto reg: program.bindings.user_data_registers) {
			if (reg < program.user_data_base ||
			    reg - program.user_data_base >= snapshot.user_data.size()) {
				if (error != nullptr) {
					*error = fmt::format("runtime snapshot is missing user SGPR {}", reg);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto alias = program.info.buffers[i].image_alias;
		if (alias != BufferResource::NoImageAlias && alias >= program.info.images.size()) {
			if (error != nullptr) {
				*error = fmt::format("buffer resource {} has invalid image alias {}", i, alias);
			}
			return false;
		}
	}
	const auto CheckWidth = [&](const auto& values, uint32_t width, const char* kind) {
		for (uint32_t i = 0; i < values.size(); i++) {
			if (values[i].dword_count != width) {
				if (error != nullptr) {
					*error = fmt::format("{} descriptor {} has {} dwords", kind, i,
					                     values[i].dword_count);
				}
				return false;
			}
		}
		return true;
	};
	for (uint32_t i = 0; i < snapshot.addresses.size(); i++) {
		if (snapshot.addresses[i].binding_base > snapshot.addresses[i].guest_base) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} binds above its guest base", i);
			}
			return false;
		}
	}
	return CheckWidth(snapshot.buffers, 4, "buffer") && CheckWidth(snapshot.images, 8, "image") &&
	       CheckWidth(snapshot.samplers, 4, "sampler");
}

bool ValidateResourceSpecialization(const Program& program, const ResourceSnapshot& snapshot,
                                    std::string* error) {
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
		return false;
	}
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto&          buffer = program.info.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		if (buffer.packed_stride != descriptor.PackedStride() ||
		    buffer.descriptor_format != descriptor.Format()) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} no longer matches specialization", i);
			}
			return false;
		}
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image      = program.info.images[i];
		const auto& descriptor = snapshot.images[i];
		if (NullImageDescriptor(descriptor)) {
			bool canonical_kind =
			    image.kind == ResourceKind::Image || image.kind == ResourceKind::StorageImage;
			if (image.atomic) {
				canonical_kind = image.kind == ResourceKind::StorageImageUint;
			}
			if (image.dimension != Decoder::ImageDimension::Dim2D || image.cube ||
			    !canonical_kind) {
				if (error != nullptr) {
					*error = fmt::format(
					    "image descriptor {} no longer matches canonical null specialization", i);
				}
				return false;
			}
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor, image.dimension);
		if (dimension == Decoder::ImageDimension::Unknown || dimension != image.dimension ||
		    DescriptorIsCube(descriptor) != image.cube) {
			if (error != nullptr) {
				*error =
				    fmt::format("image descriptor {} no longer matches specialized dimension: "
				                "{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
				                i, descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2],
				                descriptor.dwords[3], descriptor.dwords[4], descriptor.dwords[5],
				                descriptor.dwords[6], descriptor.dwords[7]);
			}
			return false;
		}
		if (image.kind == ResourceKind::Image || image.kind == ResourceKind::ImageUint ||
		    image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			const bool storage = image.kind == ResourceKind::StorageImage ||
			                     image.kind == ResourceKind::StorageImageUint;
			if (storage && image.storage_swizzle != DescriptorImageSwizzle(descriptor)) {
				if (error != nullptr) {
					*error = fmt::format("storage image descriptor {} changed swizzle", i);
				}
				return false;
			}
			const auto format = (descriptor.dwords[1] >> 20u) & 0x1ffu;
			const bool raw_sint_storage =
			    storage && format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32SInt) &&
			    !image.read && !image.atomic;
			const bool uint_descriptor = Prospero::IsUintTextureFormat(format) || raw_sint_storage;
			const auto uint_program    = image.kind == ResourceKind::ImageUint ||
			                             image.kind == ResourceKind::StorageImageUint;
			if (uint_descriptor != uint_program && !(image.atomic && uint_program)) {
				if (error != nullptr) {
					*error = fmt::format(
					    "image descriptor {} no longer matches specialized format: "
					    "{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}",
					    i, descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2],
					    descriptor.dwords[3], descriptor.dwords[4], descriptor.dwords[5],
					    descriptor.dwords[6], descriptor.dwords[7]);
				}
				return false;
			}
			if (image.depth_compare && uint_program) {
				if (error != nullptr) {
					*error = fmt::format("integer image descriptor {} uses depth comparison", i);
				}
				return false;
			}
		}
	}
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		if (program.info.addresses[i].specialized_base !=
		    AddressSpecialization(program.info.addresses[i], snapshot.addresses[i])) {
			if (error != nullptr) {
				*error = fmt::format("address resource {} no longer matches specialization", i);
			}
			return false;
		}
	}
	return true;
}

bool MaterializeResources(const Program& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, std::string* error) {
	if (!program.resource_tracking_complete) {
		if (error != nullptr) {
			*error = "shader resources were not tracked";
		}
		return false;
	}

	std::vector<DescriptorSourceRequest> requests;
	requests.reserve(program.info.buffers.size() + program.info.images.size() +
	                 program.info.samplers.size() + program.info.addresses.size());
	for (const auto& buffer: program.info.buffers) {
		requests.push_back({buffer.source, buffer.first_use_pc});
	}
	for (const auto& image: program.info.images) {
		requests.push_back({image.source, image.first_use_pc});
	}
	for (const auto& sampler: program.info.samplers) {
		requests.push_back({sampler.source, sampler.first_use_pc});
	}
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			requests.push_back({address.source, address.first_use_pc});
		}
	}

	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	if (!EvaluateRuntimeSources(program, requests, runtime, values, flattened_srt, error)) {
		return false;
	}

	ResourceSnapshot next;
	auto             cursor = values.begin();
	next.buffers.assign(cursor, cursor + program.info.buffers.size());
	cursor += program.info.buffers.size();
	// Buffer descriptors were only rejected on a non-zero Type field, which lets obvious garbage through:
	// §4.1p found descriptors whose fourth dword holds a float constant (0x3f000000 = 0.5f) and whose base is
	// 0x525fffffffff or 0x550000000000, both far outside the guest address space. Those type bits happen to be
	// zero, so they passed, bound as the null buffer, and every constant read through them returned zero -
	// which is what corrupts the base-pass VS position while its vertex attributes stay correct.
	//
	// Images already get a real validity check (ValidImageDescriptor below); buffers now get the same treatment.
	// A base beyond the guest address space cannot be a real allocation, so zero the descriptor as invalid
	// rather than binding it.
	//
	// The SRT read trace came back empty for the offending shaders, so these descriptors involve no memory
	// indirection at all - they are assembled straight from the user-data SGPR window. The garbage therefore
	// comes from the window's own contents, so the probe dumps it: the descriptor dwords should be findable
	// somewhere in it, which shows what offset was actually used and where the real descriptor sits.
	static std::atomic<uint64_t> bad_count {0};
	static std::atomic<uint64_t> good_count {0};
	for (uint32_t i = 0; i < next.buffers.size(); i++) {
		auto&                descriptor = next.buffers[i];
		ShaderBufferResource buffer;
		const bool           decoded    = DecodeBufferDescriptor(descriptor, buffer);
		const bool           wrong_type = decoded && buffer.Type() != 0;
		// 2^40 covers every guest allocation seen in practice; the observed valid bases sit around
		// 0x11'xxxxxxxx and 0x20'xxxxxxxx, while every garbage one exceeded this.
		const bool implausible_base = decoded && buffer.Base48() >= (uint64_t {1} << 40u);
		if (wrong_type || implausible_base) {
			if (implausible_base) {
				// Frequency matters as much as content now. The guest encoding at pc 0x5d0 really does name
				// s[12:15] as the V#, and the shader never writes those registers, so it expects user data
				// there - yet the shadow holds float constants. If that happens on *every* materialisation of
				// this shader it is a mapping error; if only sometimes, the shadow is stale relative to the
				// draw and the fault is ordering. Count both cases to tell them apart.
				const auto n = bad_count.fetch_add(1, std::memory_order_relaxed) + 1;
				if (n % 500 == 0) {
					std::fprintf(stderr,
					             "[bad-descriptor-rate] bad=%llu good=%llu\n",
					             static_cast<unsigned long long>(n),
					             static_cast<unsigned long long>(
					                 good_count.load(std::memory_order_relaxed)));
					std::fflush(stderr);
				}
				static std::atomic<uint32_t> log_count {0};
				if (log_count.fetch_add(1, std::memory_order_relaxed) < 4) {
					std::fprintf(stderr,
					             "[bad-descriptor] shader=0x%016" PRIx64 " buffer=%" PRIu32
					             " base=0x%012" PRIx64 " dwords=%08" PRIx32 ",%08" PRIx32
					             ",%08" PRIx32 ",%08" PRIx32 " first_use_pc=0x%08" PRIx32
					             " user_data_base=%" PRIu32 " words=%zu\n",
					             program.shader_hash, i, buffer.Base48(), descriptor.dwords[0],
					             descriptor.dwords[1], descriptor.dwords[2], descriptor.dwords[3],
					             program.info.buffers[i].first_use_pc, program.user_data_base,
					             runtime.user_data.size());
					for (size_t w = 0; w < runtime.user_data.size(); w += 8) {
						std::fprintf(stderr, "[bad-descriptor]   ud[%02zu]:", w);
						for (size_t k = w; k < w + 8 && k < runtime.user_data.size(); k++) {
							std::fprintf(stderr, " %08" PRIx32, runtime.user_data[k]);
						}
						std::fprintf(stderr, "\n");
					}

					// The user data itself is correct - ud[4..7] holds four float constants (1.4285, 0.7,
					// small magnitudes, -0.0) while every other quad is a clean V# with dword3 = 0x0004dfac.
					// So the guest laid out [V#][4 floats][V#][V#][V#][V#] and the recompiler decided a
					// descriptor lives where the constants are. Print how each descriptor dword was derived,
					// which is what identifies the mis-tracking.
					// Must go through GetDescriptorSource: descriptor ids are offset by two because 0 and 1
					// are reserved for Undefined and Unknown, so indexing provenance.descriptors directly
					// reads a different resource's chain entirely.
					// Print the chain for *every* buffer, not just the bad one. The guest's own V# quads sit
					// at user SGPRs 8-11, 16-19, 20-23, 24-27 and 28-31, with four float constants at
					// s12-15. The tracker chose s12-15 for buffer 2, which is not one of the guest's quads -
					// so seeing the whole set shows whether it is walking quads consecutively (s8, s12,
					// s16, ...) and thus skipping the gap the guest left.
					for (uint32_t b = 0; b < program.info.buffers.size(); b++) {
						const auto  src   = program.info.buffers[b].source;
						const auto* entry = GetDescriptorSource(program, src);
						if (entry == nullptr) {
							continue;
						}
						std::fprintf(stderr,
						             "[bad-descriptor]   buf%" PRIu32 " source=%" PRIu32 " pc=0x%08" PRIx32
						             " fmt=%d",
						             b, src, program.info.buffers[b].first_use_pc,
						             program.info.buffers[b].formatted ? 1 : 0);
						for (uint32_t d = 0; d < entry->dword_count && d < 4; d++) {
							std::fprintf(
							    stderr, " %s",
							    ScalarValueToString(program.provenance, entry->dwords[d]).c_str());
						}
						std::fprintf(stderr, "\n");
					}
					std::fflush(stderr);

					// Dump the guest shader so the instruction at the first-use pc can be disassembled with
					// _Build/v3/shader_disasm.exe. Four of the five buffers resolve onto real guest descriptor
					// quads and only this one points at the constants block, so this is a single mis-resolved
					// operand rather than a layout error - and the encoding at that pc is the ground truth for
					// which SGPR the instruction actually names. 8 KB comfortably covers the first-use pcs seen
					// (largest 0x8fc). Reading guest memory directly is what the SRT walker already does.
					static std::atomic<bool> dumped {false};
					bool                     expected = false;
					if (dumped.compare_exchange_strong(expected, true) && runtime.shader_base != 0) {
						const auto  path = fmt::format("_Shaders/bad_{:016x}.bin", program.shader_hash);
						std::FILE*  out  = std::fopen(path.c_str(), "wb");
						if (out != nullptr) {
							std::fwrite(reinterpret_cast<const void*>(runtime.shader_base), 1, 8192, out);
							std::fclose(out);
							std::fprintf(stderr, "[bad-descriptor]   dumped shader to %s base=0x%012" PRIx64
							                     "\n",
							             path.c_str(), runtime.shader_base);
							std::fflush(stderr);
						}
					}
				}
			}
			bool restored = false;
			if (!DisableDescriptorFallback()) {
				std::scoped_lock lock(g_last_good_mutex);
				const auto       key = std::make_pair(program.shader_hash, i);
				if (const auto found = g_last_good_descriptors.find(key);
				    found != g_last_good_descriptors.end()) {
					descriptor = found->second;
					restored   = true;
				}
			}
			if (!restored) {
				descriptor.dwords.fill(0);
			}
			TallyDescriptor(program.shader_hash, i, false);
		} else if (decoded && buffer.Base48() != 0) {
			good_count.fetch_add(1, std::memory_order_relaxed);
			TallyDescriptor(program.shader_hash, i, true);
			if (!DisableDescriptorFallback()) {
				std::scoped_lock lock(g_last_good_mutex);
				g_last_good_descriptors[std::make_pair(program.shader_hash, i)] = descriptor;
			}
		}
	}
	next.images.assign(cursor, cursor + program.info.images.size());
	cursor += program.info.images.size();
	for (auto& descriptor: next.images) {
		if (!ValidImageDescriptor(descriptor)) {
			descriptor.dwords.fill(0);
		}
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	cursor += program.info.samplers.size();
	for (const auto& address: program.info.addresses) {
		if (address.source != ScalarProvenance::Unknown) {
			const auto value = *cursor++;
			auto       base  = (static_cast<uint64_t>(value.dwords[0]) |
			                    static_cast<uint64_t>(value.dwords[1]) << 32u) &
			                   AddressMask;
			if (address.kind == ResourceKind::ScalarBuffer) {
				base &= ~uint64_t {3};
			}
			const auto before = static_cast<uint64_t>(-static_cast<int64_t>(address.min_offset));
			uint64_t   binding_base = 0;
			if (address.kind == ResourceKind::Flat) {
				binding_base = base & ~(FlatAddressWindowSize - 1u);
			} else if (base >= before) {
				binding_base = base - before;
			}
			next.addresses.push_back({base, binding_base});
		} else {
			if (!runtime.flat_memory_base.has_value()) {
				if (error != nullptr) {
					*error =
					    fmt::format("unbased {} address at pc 0x{:08x} requires runtime "
					                "guest-address translation",
					                address.kind == ResourceKind::Flat ? "FLAT" : "global/scratch",
					                address.first_use_pc);
				}
				return false;
			}
			next.addresses.push_back({*runtime.flat_memory_base, *runtime.flat_memory_base});
		}
	}
	next.flattened_srt = std::move(flattened_srt);
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	if (!ValidateResourceSnapshot(program, next, error)) {
		return false;
	}
	snapshot = std::move(next);
	return true;
}

bool SpecializeResources(Program& program, const ResourceSnapshot& snapshot, std::string* error) {
	if (!program.resource_tracking_complete || program.shader_info_complete ||
	    program.binding_layout_complete) {
		if (error != nullptr) {
			*error = !program.resource_tracking_complete ? "shader resources were not tracked"
			                                             : "resource specialization is too late";
		}
		return false;
	}
	if (!ValidateResourceSnapshot(program, snapshot, error)) {
		return false;
	}

	auto next = program.info;
	for (uint32_t i = 0; i < next.buffers.size(); i++) {
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], descriptor)) {
			if (error != nullptr) {
				*error = fmt::format("buffer descriptor {} has invalid width", i);
			}
			return false;
		}
		next.buffers[i].packed_stride     = descriptor.PackedStride();
		next.buffers[i].descriptor_format = descriptor.Format();
	}
	for (uint32_t i = 0; i < next.addresses.size(); i++) {
		next.addresses[i].specialized_base =
		    AddressSpecialization(next.addresses[i], snapshot.addresses[i]);
	}
	for (uint32_t i = 0; i < next.images.size(); i++) {
		const auto& descriptor = snapshot.images[i];
		auto&       image      = next.images[i];
		if (NullImageDescriptor(descriptor)) {
			image.dimension = Decoder::ImageDimension::Dim2D;
			image.cube      = false;
			switch (image.kind) {
				case ResourceKind::ImageUint: image.kind = ResourceKind::Image; break;
				case ResourceKind::StorageImageUint:
					if (!image.atomic) {
						image.kind = ResourceKind::StorageImage;
					}
					break;
				default: break;
			}
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor, image.dimension);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			if (error != nullptr) {
				*error = fmt::format(
				    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
				    "{:08x},{:08x},{:08x},{:08x}",
				    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0],
				    descriptor.dwords[1], descriptor.dwords[2], descriptor.dwords[3],
				    descriptor.dwords[4], descriptor.dwords[5], descriptor.dwords[6],
				    descriptor.dwords[7]);
			}
			return false;
		}
		image.dimension = descriptor_dimension;
		image.cube      = DescriptorIsCube(descriptor);
		if (image.kind == ResourceKind::StorageImage ||
		    image.kind == ResourceKind::StorageImageUint) {
			image.storage_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const auto format  = (descriptor.dwords[1] >> 20u) & 0x1ffu;
		const bool storage = image.kind == ResourceKind::StorageImage ||
		                     image.kind == ResourceKind::StorageImageUint;
		const bool raw_sint_storage =
		    storage && format == Prospero::GpuEnumValue(Prospero::BufferFormat::k32SInt) &&
		    !image.read && !image.atomic;
		const bool uint_image = Prospero::IsUintTextureFormat(format) || raw_sint_storage;
		if (uint_image) {
			switch (image.kind) {
				case ResourceKind::Image: image.kind = ResourceKind::ImageUint; break;
				case ResourceKind::StorageImage: image.kind = ResourceKind::StorageImageUint; break;
				default: break;
			}
		}
	}
	struct ImagePatch {
		std::reference_wrapper<Instruction> inst;
		ResourceKind                        kind;
		Decoder::ImageDimension             dimension;
		bool                                cube;
	};
	std::vector<ImagePatch> patches;
	for (auto& block: program.blocks) {
		for (auto& inst: block.instructions) {
			if (inst.memory.kind != ResourceKind::Image &&
			    inst.memory.kind != ResourceKind::ImageUint &&
			    inst.memory.kind != ResourceKind::StorageImage &&
			    inst.memory.kind != ResourceKind::StorageImageUint) {
				continue;
			}
			if (inst.memory.resource >= next.images.size()) {
				if (error != nullptr) {
					*error = fmt::format("image instruction at pc 0x{:08x} has invalid resource {}",
					                     inst.pc, inst.memory.resource);
				}
				return false;
			}
			const auto& image = next.images[inst.memory.resource];
			patches.push_back({std::ref(inst), image.kind, image.dimension, image.cube});
		}
	}
	program.info = std::move(next);
	for (const auto& patch: patches) {
		patch.inst.get().memory.kind            = patch.kind;
		patch.inst.get().memory.image_dimension = patch.dimension;
		patch.inst.get().memory.image_cube      = patch.cube;
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
