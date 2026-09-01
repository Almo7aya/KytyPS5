#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_

#include <cstdint>

namespace Libs::Graphics {

class GpuResourceManager;
struct ShaderVertexInputBuffer;
struct ShaderVertexInputInfo;

[[nodiscard]] int32_t  ResolveVertexOffset(uint32_t                     index_offset,
                                           const ShaderVertexInputInfo& vs_input_info);
[[nodiscard]] uint32_t ResolveInstanceOffset(const ShaderVertexInputInfo& vs_input_info);
[[nodiscard]] uint64_t ResolveVertexBufferRequestSize(
    const GpuResourceManager& resources, const ShaderVertexInputBuffer& vertex);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
