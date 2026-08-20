#pragma once

/**
 * @file live2d_draw.h
 * @brief A posed model into the frame's mesh geometry (namespace nxm::live2d).
 */

#include "core/rendering/render2d/mesh_channel.h"
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

usize emit_model(const ModelAsset &asset, const ModelView &view,
                 nxe::r2d::MeshChannel &out);

/// How one emitted draw is clipped. One per draw appended, in the same order.
struct DrawMask {
  i32 group = -1;
  u32 atlas = 0;
  u32 channel = 0;
  bool inverted = false;
  glm::mat3 from_world{1.f};

  [[nodiscard]] bool clipped() const noexcept { return group >= 0; }
};

/// As above, and additionally fills @p out_masks with one entry per appended
/// draw, so a pass that can clip knows which of them to and how.
usize emit_model(const ModelAsset &asset, const ModelView &view,
                 const MaskLayout &masks, nxe::r2d::MeshChannel &out,
                 nx::vector<DrawMask> &out_masks, u32 atlas_base = 0);

/// One mask group's geometry, in model space, to be drawn into the atlas.
struct MaskDraw {
  u32 first_index = 0;
  u32 index_count = 0;
  u32 vertex_offset = 0;
  u32 texture = 0;
  u32 atlas = 0;
  u32 channel = 0;
  glm::mat3 to_mask{1.f};
  glm::vec4 tile{-1.f, -1.f, 1.f, 1.f};
};

struct MaskChannel {
  nx::vector<nxe::r2d::MeshVertex> vertices;
  nx::vector<u32> indices;
  nx::vector<MaskDraw> draws;

  [[nodiscard]] bool empty() const noexcept { return draws.empty(); }
  void clear() {
    vertices.clear();
    indices.clear();
    draws.clear();
  }
};

/// Appends the shapes making up each of @p masks' groups. Model space, because
/// a mask is drawn into its own tile and never through a camera.
usize emit_masks(const ModelAsset &asset, const MaskLayout &masks,
                 MaskChannel &out, u32 atlas_base = 0);

[[nodiscard]] usize masked_drawable_count(const ModelAsset &asset) noexcept;

} // namespace nxm::live2d
