#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace Libs::Graphics {

namespace {

struct LayoutKey {
	uint64_t vs_shader_id = 0;
	uint64_t ps_shader_id = 0;

	bool operator==(const LayoutKey&) const = default;
};

struct LayoutKeyHash {
	std::size_t operator()(const LayoutKey& key) const {
		std::size_t hash = 0;
		Mix(hash, key.vs_shader_id);
		Mix(hash, key.ps_shader_id);
		return hash;
	}

	static void Mix(std::size_t& hash, uint64_t value) {
		hash ^= static_cast<std::size_t>(value) + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) +
		        (hash << 6u) + (hash >> 2u);
		if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
			hash ^= static_cast<std::size_t>(value >> 32u) + (hash << 6u) + (hash >> 2u);
		}
	}
};

struct LibraryKey {
	PipelineRenderingState   rendering {};
	uint64_t                 vs_shader_id = 0;
	uint64_t                 ps_shader_id = 0;
	PipelineStaticParameters static_params {};

	bool operator==(const LibraryKey& other) const {
		return rendering == other.rendering && vs_shader_id == other.vs_shader_id &&
		       ps_shader_id == other.ps_shader_id && static_params == other.static_params;
	}
};

struct LibraryKeyHash {
	std::size_t operator()(const LibraryKey& key) const {
		std::size_t hash = 0;
		LayoutKeyHash::Mix(hash, key.vs_shader_id);
		LayoutKeyHash::Mix(hash, key.ps_shader_id);
		LayoutKeyHash::Mix(hash, key.rendering.color_count);
		for (const auto format: key.rendering.color_formats) {
			LayoutKeyHash::Mix(hash, static_cast<uint32_t>(format));
		}
		LayoutKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.depth_format));
		LayoutKeyHash::Mix(hash, static_cast<uint32_t>(key.rendering.stencil_format));
		const auto* bytes = reinterpret_cast<const uint8_t*>(&key.static_params);
		for (std::size_t i = 0; i < sizeof(key.static_params); i++) {
			LayoutKeyHash::Mix(hash, bytes[i]);
		}
		return hash;
	}
};

PipelineStaticParameters VertexInputParams(const PipelineStaticParameters& source) {
	PipelineStaticParameters result {};
	result.topology                 = source.topology;
	result.primitive_restart_enable = source.primitive_restart_enable;
	return result;
}

PipelineStaticParameters PreRasterParams(const PipelineStaticParameters& source) {
	PipelineStaticParameters result {};
	result.negative_one_to_one = source.negative_one_to_one;
	result.depth_clip_enable   = source.depth_clip_enable;
	result.topology            = source.topology;
	result.cull_front          = source.cull_front;
	result.cull_back           = source.cull_back;
	result.face                = source.face;
	return result;
}

PipelineStaticParameters FragmentShaderParams(const PipelineStaticParameters& source) {
	PipelineStaticParameters result {};
	result.samples                  = source.samples;
	result.sample_shading_enable    = source.sample_shading_enable;
	result.with_depth               = source.with_depth;
	result.depth_test_enable        = source.depth_test_enable;
	result.depth_write_enable       = source.depth_write_enable;
	result.depth_compare_op         = source.depth_compare_op;
	result.depth_bounds_test_enable = source.depth_bounds_test_enable;
	result.depth_min_bounds         = source.depth_min_bounds;
	result.depth_max_bounds         = source.depth_max_bounds;
	result.stencil_test_enable      = source.stencil_test_enable;
	result.stencil_front            = source.stencil_front;
	result.stencil_back             = source.stencil_back;
	return result;
}

PipelineStaticParameters FragmentOutputParams(const PipelineStaticParameters& source) {
	PipelineStaticParameters result {};
	result.samples               = source.samples;
	result.sample_shading_enable = source.sample_shading_enable;
	result.color_count           = source.color_count;
	for (uint32_t i = 0; i < RENDER_COLOR_ATTACHMENTS_MAX; i++) {
		result.color_mask[i]           = source.color_mask[i];
		result.color_srcblend[i]       = source.color_srcblend[i];
		result.color_comb_fcn[i]       = source.color_comb_fcn[i];
		result.color_destblend[i]      = source.color_destblend[i];
		result.alpha_srcblend[i]       = source.alpha_srcblend[i];
		result.alpha_comb_fcn[i]       = source.alpha_comb_fcn[i];
		result.alpha_destblend[i]      = source.alpha_destblend[i];
		result.separate_alpha_blend[i] = source.separate_alpha_blend[i];
		result.blend_enable[i]         = source.blend_enable[i];
		result.blend_bypass[i]         = source.blend_bypass[i];
	}
	return result;
}

} // namespace

struct GraphicsPipelineLibraryCache::Impl {
	struct Layout {
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		vk::PipelineLayout      pipeline_layout       = nullptr;
		bool                    uses_push_descriptors = false;
	};

	using LibraryMap = std::unordered_map<LibraryKey, vk::Pipeline, LibraryKeyHash>;

	Impl(GraphicContext& graphics, vk::PipelineCache driver_cache)
	    : graphics(graphics), driver_cache(driver_cache) {}

	vk::Pipeline CreateLibrary(LibraryMap& libraries, const LibraryKey& key,
	                           const vk::GraphicsPipelineCreateInfo& create_info) {
		if (const auto iter = libraries.find(key); iter != libraries.end()) {
			return iter->second;
		}
		vk::Pipeline pipeline = nullptr;
		const auto   result = graphics.device.createGraphicsPipelines(driver_cache, 1, &create_info,
		                                                              nullptr, &pipeline);
		if (result != vk::Result::eSuccess || pipeline == nullptr) {
			LOGF("Graphics pipeline library creation failed: %s; using monolithic pipelines\n",
			     VulkanToString(result).c_str());
			if (pipeline != nullptr) {
				graphics.device.destroyPipeline(pipeline, nullptr);
			}
			failed = true;
			return nullptr;
		}
		libraries.emplace(key, pipeline);
		return pipeline;
	}

	GraphicContext&                                      graphics;
	vk::PipelineCache                                    driver_cache = nullptr;
	bool                                                 failed       = false;
	std::unordered_map<LayoutKey, Layout, LayoutKeyHash> layouts;
	LibraryMap                                           vertex_input_libraries;
	LibraryMap                                           pre_raster_libraries;
	LibraryMap                                           fragment_shader_libraries;
	LibraryMap                                           fragment_output_libraries;
};

GraphicsPipelineLibraryCache::GraphicsPipelineLibraryCache(GraphicContext&   graphics,
                                                           vk::PipelineCache driver_cache)
    : m_impl(std::make_unique<Impl>(graphics, driver_cache)) {}

GraphicsPipelineLibraryCache::~GraphicsPipelineLibraryCache() {
	auto destroy_libraries = [this](const Impl::LibraryMap& libraries) {
		for (const auto& [key, pipeline]: libraries) {
			(void)key;
			m_impl->graphics.device.destroyPipeline(pipeline, nullptr);
		}
	};
	destroy_libraries(m_impl->vertex_input_libraries);
	destroy_libraries(m_impl->pre_raster_libraries);
	destroy_libraries(m_impl->fragment_shader_libraries);
	destroy_libraries(m_impl->fragment_output_libraries);
	for (const auto& [key, layout]: m_impl->layouts) {
		(void)key;
		m_impl->graphics.device.destroyPipelineLayout(layout.pipeline_layout, nullptr);
		m_impl->graphics.device.destroyDescriptorSetLayout(layout.descriptor_set_layout, nullptr);
	}
}

void GraphicsPipelineLibraryCache::PrepareLayout(
    PipelineCache::GraphicsPipeline&                pipeline,
    std::span<const vk::DescriptorSetLayoutBinding> bindings) {
	const LayoutKey key {pipeline.vs_shader_id, pipeline.ps_shader_id};
	auto            iter = m_impl->layouts.find(key);
	if (iter == m_impl->layouts.end()) {
		Impl::Layout layout {};
		uint32_t     descriptor_count = 0;
		for (const auto& binding: bindings) {
			descriptor_count += binding.descriptorCount;
		}
		layout.uses_push_descriptors = descriptor_count <= m_impl->graphics.max_push_descriptors;

		vk::DescriptorSetLayoutCreateInfo descriptor_info {};
		descriptor_info.sType = vk::StructureType::eDescriptorSetLayoutCreateInfo;
		descriptor_info.flags = layout.uses_push_descriptors
		                            ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
		                            : vk::DescriptorSetLayoutCreateFlags {};
		descriptor_info.bindingCount = static_cast<uint32_t>(bindings.size());
		descriptor_info.pBindings    = bindings.data();
		EXIT_IF(m_impl->graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
		                                                          &layout.descriptor_set_layout) !=
		        vk::Result::eSuccess);

		constexpr auto GraphicsStages =
		    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
		const vk::PushConstantRange  push_constants {GraphicsStages, 0,
		                                             ShaderRecompiler::IR::NativePushConstantSize};
		vk::PipelineLayoutCreateInfo pipeline_layout_info {};
		pipeline_layout_info.sType                  = vk::StructureType::ePipelineLayoutCreateInfo;
		pipeline_layout_info.setLayoutCount         = 1;
		pipeline_layout_info.pSetLayouts            = &layout.descriptor_set_layout;
		pipeline_layout_info.pushConstantRangeCount = 1;
		pipeline_layout_info.pPushConstantRanges    = &push_constants;
		const auto result                           = m_impl->graphics.device.createPipelineLayout(
		    &pipeline_layout_info, nullptr, &layout.pipeline_layout);
		if (result != vk::Result::eSuccess) {
			m_impl->graphics.device.destroyDescriptorSetLayout(layout.descriptor_set_layout,
			                                                   nullptr);
			EXIT("Could not create graphics pipeline-library layout: %s\n",
			     VulkanToString(result).c_str());
		}
		iter = m_impl->layouts.emplace(key, layout).first;
	}

	pipeline.descriptor_set_layout = iter->second.descriptor_set_layout;
	pipeline.pipeline_layout       = iter->second.pipeline_layout;
	pipeline.uses_push_descriptors = iter->second.uses_push_descriptors;
	pipeline.owns_layout           = false;
}

bool GraphicsPipelineLibraryCache::CreatePipeline(
    PipelineCache::GraphicsPipeline& pipeline, const PipelineRenderingState& rendering,
    const PipelineStaticParameters&       static_params,
    const vk::GraphicsPipelineCreateInfo& pipeline_info, uint32_t pre_raster_stage_count,
    const vk::PipelineShaderStageCreateInfo* fragment_stage) {
	if (m_impl->failed) {
		return false;
	}

	auto make_library_info = [](vk::GraphicsPipelineLibraryFlagsEXT flags, const void* next) {
		vk::GraphicsPipelineLibraryCreateInfoEXT info {};
		info.sType = vk::StructureType::eGraphicsPipelineLibraryCreateInfoEXT;
		info.pNext = next;
		info.flags = flags;
		return info;
	};

	LibraryKey vertex_key {};
	vertex_key.vs_shader_id  = pipeline.vs_shader_id;
	vertex_key.static_params = VertexInputParams(static_params);
	auto vertex_subset =
	    make_library_info(vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface, nullptr);
	auto vertex_info                = pipeline_info;
	vertex_info.pNext               = &vertex_subset;
	vertex_info.flags               = vk::PipelineCreateFlagBits::eLibraryKHR;
	vertex_info.stageCount          = 0;
	vertex_info.pStages             = nullptr;
	vertex_info.pTessellationState  = nullptr;
	vertex_info.pViewportState      = nullptr;
	vertex_info.pRasterizationState = nullptr;
	vertex_info.pMultisampleState   = nullptr;
	vertex_info.pDepthStencilState  = nullptr;
	vertex_info.pColorBlendState    = nullptr;
	vertex_info.pDynamicState       = nullptr;
	vertex_info.layout              = nullptr;
	const auto vertex_library =
	    m_impl->CreateLibrary(m_impl->vertex_input_libraries, vertex_key, vertex_info);
	if (vertex_library == nullptr) {
		return false;
	}

	LibraryKey pre_raster_key {};
	pre_raster_key.vs_shader_id  = pipeline.vs_shader_id;
	pre_raster_key.ps_shader_id  = pipeline.ps_shader_id;
	pre_raster_key.static_params = PreRasterParams(static_params);
	auto pre_raster_subset       = make_library_info(
	    vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders, pipeline_info.pNext);
	auto pre_raster_info                = pipeline_info;
	pre_raster_info.pNext               = &pre_raster_subset;
	pre_raster_info.flags               = vk::PipelineCreateFlagBits::eLibraryKHR;
	pre_raster_info.stageCount          = pre_raster_stage_count;
	pre_raster_info.pVertexInputState   = nullptr;
	pre_raster_info.pInputAssemblyState = nullptr;
	pre_raster_info.pMultisampleState   = nullptr;
	pre_raster_info.pDepthStencilState  = nullptr;
	pre_raster_info.pColorBlendState    = nullptr;
	LibraryKey fragment_key {};
	fragment_key.vs_shader_id  = pipeline.vs_shader_id;
	fragment_key.ps_shader_id  = pipeline.ps_shader_id;
	fragment_key.static_params = FragmentShaderParams(static_params);
	auto fragment_subset       = make_library_info(
	    vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader, pipeline_info.pNext);
	auto fragment_info                = pipeline_info;
	fragment_info.pNext               = &fragment_subset;
	fragment_info.flags               = vk::PipelineCreateFlagBits::eLibraryKHR;
	fragment_info.stageCount          = fragment_stage == nullptr ? 0u : 1u;
	fragment_info.pStages             = fragment_stage;
	fragment_info.pVertexInputState   = nullptr;
	fragment_info.pInputAssemblyState = nullptr;
	fragment_info.pTessellationState  = nullptr;
	fragment_info.pViewportState      = nullptr;
	fragment_info.pRasterizationState = nullptr;
	fragment_info.pColorBlendState    = nullptr;
	const auto pre_raster_library =
	    m_impl->CreateLibrary(m_impl->pre_raster_libraries, pre_raster_key, pre_raster_info);
	const auto fragment_library =
	    m_impl->CreateLibrary(m_impl->fragment_shader_libraries, fragment_key, fragment_info);
	if (pre_raster_library == nullptr || fragment_library == nullptr) {
		return false;
	}

	LibraryKey output_key {};
	output_key.rendering     = rendering;
	output_key.static_params = FragmentOutputParams(static_params);
	auto output_subset       = make_library_info(
	    vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface, pipeline_info.pNext);
	auto output_info                = pipeline_info;
	output_info.pNext               = &output_subset;
	output_info.flags               = vk::PipelineCreateFlagBits::eLibraryKHR;
	output_info.stageCount          = 0;
	output_info.pStages             = nullptr;
	output_info.pVertexInputState   = nullptr;
	output_info.pInputAssemblyState = nullptr;
	output_info.pTessellationState  = nullptr;
	output_info.pViewportState      = nullptr;
	output_info.pRasterizationState = nullptr;
	output_info.pDepthStencilState  = nullptr;
	output_info.layout              = nullptr;
	const auto output_library =
	    m_impl->CreateLibrary(m_impl->fragment_output_libraries, output_key, output_info);
	if (output_library == nullptr) {
		return false;
	}

	const std::array libraries {vertex_library, pre_raster_library, fragment_library,
	                            output_library};
	vk::PipelineLibraryCreateInfoKHR link_info {};
	link_info.sType        = vk::StructureType::ePipelineLibraryCreateInfoKHR;
	link_info.libraryCount = static_cast<uint32_t>(libraries.size());
	link_info.pLibraries   = libraries.data();
	vk::GraphicsPipelineCreateInfo link_pipeline_info {};
	link_pipeline_info.sType  = vk::StructureType::eGraphicsPipelineCreateInfo;
	link_pipeline_info.pNext  = &link_info;
	link_pipeline_info.layout = pipeline.pipeline_layout;
	const auto result         = m_impl->graphics.device.createGraphicsPipelines(
	    m_impl->driver_cache, 1, &link_pipeline_info, nullptr, &pipeline.pipeline);
	if (result != vk::Result::eSuccess || pipeline.pipeline == nullptr) {
		LOGF("Graphics pipeline library link failed: %s; using monolithic pipelines\n",
		     VulkanToString(result).c_str());
		if (pipeline.pipeline != nullptr) {
			m_impl->graphics.device.destroyPipeline(pipeline.pipeline, nullptr);
			pipeline.pipeline = nullptr;
		}
		m_impl->failed = true;
		return false;
	}
	return true;
}

} // namespace Libs::Graphics
