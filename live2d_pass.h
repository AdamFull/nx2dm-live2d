#pragma once

#include "live2d/live2d_draw.h"
#include "rendering/graph/render_graph.h"
#include "rendering/rhi/descs.h"
#include "rendering/rhi/upload_ring.h"

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

inline constexpr u32 MAX_MASK_ATLASES = 16;
inline constexpr u32 NO_MASK = 0xFFFFFFFFu;

// Mirrors Live2DDraw in live2d.slang. A model draw reads row0/row1 as its
// world-to-atlas affine; a mask draw as its model-to-clip affine and clamps
// to tile.
struct DrawRecord {
  glm::vec4 row0{0.f};
  glm::vec4 row1{0.f};
  glm::vec4 channel{0.f};
  glm::vec4 tile{-1.f, -1.f, 1.f, 1.f};
  u32 index_offset = 0;
  u32 vertex_offset = 0;
  NxTexture2D<float4> texture{};
  u32 camera = 0;
  u32 mask = NO_MASK;
  u32 inverted = 0;
  u32 _pad0 = 0;
  u32 _pad1 = 0;
};
static_assert(sizeof(DrawRecord) == 96);

// One record and one indirect command per draw, the command naming its record
// by first_instance: the model's draws in order, then the mask draws grouped
// by atlas.
struct DrawStream {
  nx::vector<DrawRecord> records;
  nx::vector<nxe::rhi::DrawIndirectCommand> commands;
  nx::small_vector<u32, MAX_MASK_ATLASES + 1> atlas_first;

  [[nodiscard]] u32 model_count() const noexcept {
    return atlas_first.empty() ? nx::cast<u32>(records.size())
                               : atlas_first.front();
  }

  void build(const Frame &frame);
};

struct PushBlock {
  u64 cameras = 0;
  u64 vertices = 0;
  u64 indices = 0;
  u64 draws = 0;
  NxTexture2D<float4> masks[MAX_MASK_ATLASES]{};
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
    DRAWS,
    COMMANDS,
    ARRAY_COUNT
  };

  static constexpr u32 MAX_MULTI_DRAW = 65535;

  nxe::rhi::UploadRing m_ring;
  DrawStream m_stream;
  nxe::rhi::PipelineHandle m_mask_pipeline;
  nxe::rhi::PipelineHandle
      m_model_pipeline[nx::cast<usize>(nxe::r2d::MeshBlend::Count)];
  nxe::rhi::Format m_format = nxe::rhi::Format::Unknown;
  u32 m_sampler = 0;
};

} // namespace nxm::live2d
