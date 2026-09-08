#pragma once

#include "core/rendering/graph/render_graph.h"
#include "core/rendering/rhi/upload_ring.h"
#include "live2d/live2d_draw.h"

namespace nxm::live2d {

[[nodiscard]] nxe::rhi::BlendMode
pipeline_blend(nxe::r2d::MeshBlend blend) noexcept;

struct Frame {
  nxe::r2d::MeshChannel geometry;
  nx::vector<DrawMask> clips;
  MaskChannel masks;
  nx::small_vector<u32, 16> atlas_sizes;

  [[nodiscard]] bool empty() const noexcept { return geometry.draws.empty(); }

  void clear() {
    geometry.clear();
    clips.clear();
    masks.clear();
    atlas_sizes.clear();
  }
};

struct PushBlock {
  glm::vec4 mask_row0{0.f};
  glm::vec4 mask_row1{0.f};
  glm::vec4 channel{0.f};
  glm::vec4 tile{-1.f, -1.f, 1.f, 1.f};
  u64 cameras = 0;
  u64 vertices = 0;
  u64 indices = 0;
  u32 index_offset = 0;
  u32 vertex_offset = 0;
  NxTexture2D<float4> texture{};
  u32 camera = 0;
  NxTexture2D<float4> mask_texture{};
  u32 inverted = 0;
};
static_assert(sizeof(PushBlock) <= nxe::rhi::PUSH_CONSTANT_SIZE,
              "the Live2D push block must fit the guaranteed push range");

class ModelRenderer {
public:
  ModelRenderer() = default;
  ~ModelRenderer() = default;

  ModelRenderer(const ModelRenderer &) = delete;
  ModelRenderer &operator=(const ModelRenderer &) = delete;

  [[nodiscard]] bool init(nxe::rhi::Device &device, u32 sampler);
  void shutdown();

  void set_pipelines(nxe::rhi::PipelineHandle mask,
                     std::span<const nxe::rhi::PipelineHandle,
                               nx::cast<usize>(nxe::r2d::MeshBlend::Count)>
                         models,
                     nxe::rhi::Format format);

  [[nodiscard]] bool ready() const noexcept { return m_mask_pipeline.valid(); }

  void draw(nxe::rhi::Device &device, nxe::rg::RenderGraph &graph,
            nxe::rg::TextureId target, nxe::rhi::Format format, u64 cameras,
            const Frame &frame);

private:
  enum Array : u32 {
    MODEL_VERTICES,
    MODEL_INDICES,
    MASK_VERTICES,
    MASK_INDICES,
    ARRAY_COUNT
  };

  nxe::rhi::UploadRing m_ring;
  nxe::rhi::PipelineHandle m_mask_pipeline;
  nxe::rhi::PipelineHandle
      m_model_pipeline[nx::cast<usize>(nxe::r2d::MeshBlend::Count)];
  nxe::rhi::Format m_format = nxe::rhi::Format::Unknown;
  u32 m_sampler = 0;
};

} // namespace nxm::live2d
