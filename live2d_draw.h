#pragma once

#include "rendering/render2d/mesh_channel.h"
#include "live2d/live2d_assets.h"
#include "live2d/live2d_mask.h"

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <span>

namespace nxm::live2d {

struct DrawableMesh {
  std::span<const glm::vec2> positions;
  std::span<const glm::vec2> uvs;
  std::span<const u16> indices;

  [[nodiscard]] bool valid() const noexcept {
    return !positions.empty() && positions.size() == uvs.size() &&
           !indices.empty();
  }
};

[[nodiscard]] DrawableMesh drawable_mesh(const ModelAsset &asset,
                                         i32 drawable) noexcept;

[[nodiscard]] bool drawable_visible(const ModelAsset &asset,
                                    i32 drawable) noexcept;

struct ModelView {
  glm::mat3 world{1.f};
  glm::vec4 color{1.f, 1.f, 1.f, 1.f};
  i32 layer = 0;
  u32 camera = 0;
  f32 depth_min = -1024.f;
  f32 depth_max = 1024.f;
  u32 batch = 0;
  u32 material = 0;
};

/// Expands a posed model into engine mesh draws, unclipped.
usize emit_model(const ModelAsset &asset, const ModelView &view,
                 nxe::r2d::MeshChannel &out);

struct DrawMask {
  i32 group = -1;
  u32 atlas = 0;
  u32 channel = 0;
  bool inverted = false;
  /// Model space to the mask atlas's 0..1 texture space.
  glm::mat3 to_mask{1.f};

  [[nodiscard]] bool clipped() const noexcept { return group >= 0; }
};

/// One visible drawable, drawn from its model's positions and its mesh.
struct ModelDraw {
  u32 model = 0;
  u32 drawable = 0;
  u32 texture = 0;
  u32 color = 0;
  nxe::r2d::MeshBlend blend = nxe::r2d::MeshBlend::Normal;
  DrawMask clip;
};

/// Appends the model's visible drawables in render order, naming @p model.
/// Each is clipped by @p masks when given, as if its atlases came first.
usize emit_draws(const ModelAsset &asset, const ModelView &view,
                 const MaskLayout *masks, u32 model,
                 nx::vector<ModelDraw> &out);

/// Writes every drawable's current positions, in model space, in the mesh's
/// vertex order. @p out holds the mesh's vertex_count().
void copy_positions(const ModelAsset &asset, std::span<glm::vec2> out) noexcept;

/// One mask shape: a drawable drawn into its group's tile of a mask atlas.
struct MaskShape {
  u32 model = 0;
  u32 drawable = 0;
  u32 texture = 0;
  u32 atlas = 0;
  u32 channel = 0;
  /// Model space to the atlas's -1..1 clip space.
  glm::mat3 to_mask{1.f};
  glm::vec4 tile{-1.f, -1.f, 1.f, 1.f};
};

usize emit_mask_shapes(const ModelAsset &asset, const MaskLayout &masks,
                       u32 model, nx::vector<MaskShape> &out);

[[nodiscard]] usize masked_drawable_count(const ModelAsset &asset) noexcept;

}
