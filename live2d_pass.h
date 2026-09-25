#pragma once

#include "live2d/live2d_draw.h"
#include "rendering/graph/render_graph.h"
#include "rendering/rhi/descs.h"
#include "rendering/rhi/upload_ring.h"

#include <glm/vec2.hpp>

#include <span>

namespace nxm::live2d {

[[nodiscard]] nxe::rhi::BlendMode
pipeline_blend(nxe::r2d::MeshBlend blend) noexcept;

struct FrameModel {
  nx::shared_ptr<const ModelMesh> mesh;
  /// Its first vertex in Frame::positions.
  u32 positions = 0;
  glm::mat3 world{1.f};
  u32 camera = 0;
};

struct Frame {
  nx::vector<FrameModel> models;
  nx::vector<glm::vec2> positions;
  nx::vector<ModelDraw> draws;
  nx::vector<MaskShape> masks;
  nx::small_vector<u32, 16> atlas_sizes;

  [[nodiscard]] bool empty() const noexcept { return draws.empty(); }

  void clear() {
    models.clear();
    positions.clear();
    draws.clear();
    masks.clear();
    atlas_sizes.clear();
  }
};

inline constexpr u32 MAX_MASK_ATLASES = 16;
inline constexpr u32 NO_MASK = 0xFFFFFFFFu;
inline constexpr u32 DEFAULT_ATLAS_LIMIT = 2048;

// A model's static geometry on the GPU; zero until it is resident.
struct MeshAddress {
  u64 uvs = 0;
  u64 indices = 0;

  [[nodiscard]] bool resident() const noexcept {
    return uvs != 0u && indices != 0u;
  }
};

// Mirrors Live2DDraw in live2d.slang. Positions are in model space: a model
// draw reads row0/row1 as its model-to-atlas affine and world0/world1 as its
// model-to-world one; a mask draw reads row0/row1 as model-to-clip and clamps
// to tile.
struct DrawRecord {
  glm::vec4 row0{0.f};
  glm::vec4 row1{0.f};
  glm::vec4 world0{1.f, 0.f, 0.f, 0.f};
  glm::vec4 world1{0.f, 1.f, 0.f, 0.f};
  glm::vec4 channel{0.f};
  glm::vec4 tile{-1.f, -1.f, 1.f, 1.f};
  u64 uvs = 0;
  u64 indices = 0;
  u32 first_index = 0;
  u32 first_vertex = 0;
  u32 positions = 0;
  NxTexture2D<float4> texture{};
  u32 camera = 0;
  u32 mask = NO_MASK;
  u32 inverted = 0;
  u32 color = 0;
};
static_assert(sizeof(DrawRecord) == 144);

// A mask placed as a tile of a shared atlas: its own atlas's unit square maps
// to [offset, offset + scale] of the shared one.
struct MaskTile {
  u32 atlas = 0;
  glm::vec2 scale{1.f};
  glm::vec2 offset{0.f};
};

// Packs the frame's per-model mask atlases as tiles of as few shared atlases as
// fit, each no wider or taller than @p limit. Tiles of one size share atlases;
// @p tiles is indexed like frame.atlas_sizes.
void pack_mask_atlases(std::span<const u32> sizes, u32 limit,
                       nx::small_vector<MaskTile, MAX_MASK_ATLASES> &tiles,
                       nx::small_vector<glm::uvec2, MAX_MASK_ATLASES> &atlases);

// One record and one indirect command per draw, the command naming its record
// by first_instance: the models' draws in order, then the mask shapes grouped
// by shared atlas.
struct DrawStream {
  nx::vector<DrawRecord> records;
  nx::vector<nxe::rhi::DrawIndirectCommand> commands;
  nx::small_vector<u32, MAX_MASK_ATLASES + 1> atlas_first;
  nx::small_vector<MaskTile, MAX_MASK_ATLASES> tiles;
  nx::small_vector<glm::uvec2, MAX_MASK_ATLASES> atlases;

  [[nodiscard]] u32 model_count() const noexcept {
    return atlas_first.empty() ? nx::cast<u32>(records.size())
                               : atlas_first.front();
  }

  /// @p meshes is indexed like frame.models; a model whose mesh is not yet
  /// resident draws nothing, masks included.
  void build(const Frame &frame, std::span<const MeshAddress> meshes,
             u32 atlas_limit = DEFAULT_ATLAS_LIMIT);
};

struct PushBlock {
  u64 cameras = 0;
  u64 positions = 0;
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

  /// Meshes held on the GPU, uploaded once each.
  [[nodiscard]] usize cached_meshes() const noexcept { return m_meshes.size(); }

  void set_atlas_limit(u32 limit) noexcept;

  void draw(nxe::rhi::Device &device, nxe::rg::RenderGraph &graph,
            nxe::rg::TextureId target, nxe::rhi::Format format, u64 cameras,
            const Frame &frame);

private:
  enum Array : u32 { POSITIONS, DRAWS, COMMANDS, ARRAY_COUNT };

  static constexpr u32 MAX_MULTI_DRAW = 65535;
  /// Frames a mesh no frame names stays resident, past any still in flight.
  static constexpr u64 KEEP_FRAMES = 8;

  struct CachedMesh {
    nx::shared_ptr<const ModelMesh> mesh;
    nxe::rhi::BufferHandle uvs;
    nxe::rhi::BufferHandle indices;
    MeshAddress address;
    nxe::rhi::UploadTicket uv_upload;
    nxe::rhi::UploadTicket index_upload;
    bool resident = false;
    u64 used = 0;
  };

  [[nodiscard]] MeshAddress
  resolve(const nx::shared_ptr<const ModelMesh> &mesh);
  void release(CachedMesh &cached) noexcept;
  void release_unused() noexcept;

  nxe::rhi::Device *m_device = nullptr;
  nxe::rhi::UploadRing m_ring;
  DrawStream m_stream;
  nx::vector<CachedMesh> m_meshes;
  nx::vector<MeshAddress> m_addresses;
  u64 m_frame = 0;
  nxe::rhi::PipelineHandle m_mask_pipeline;
  nxe::rhi::PipelineHandle
      m_model_pipeline[nx::cast<usize>(nxe::r2d::MeshBlend::Count)];
  nxe::rhi::Format m_format = nxe::rhi::Format::Unknown;
  u32 m_sampler = 0;
  u32 m_atlas_limit = DEFAULT_ATLAS_LIMIT;
};

} // namespace nxm::live2d
