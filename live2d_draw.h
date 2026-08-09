#pragma once

/**
 * @file live2d_draw.h
 * @brief A posed model into the frame's mesh geometry (namespace nxm::live2d).
 */

#include "core/rendering/render2d/mesh_channel.h"
#include "live2d/live2d_assets.h"

#include <glm/mat3x3.hpp>
#include <glm/vec4.hpp>

namespace nxm::live2d {

struct ModelView {
  glm::mat3 world{1.f};
  glm::vec4 color{1.f, 1.f, 1.f, 1.f};
  i32 layer = 0;
  u32 camera = 0;
  f32 depth_min = -1024.f;
  f32 depth_max = 1024.f;
};

usize emit_model(const ModelAsset &asset, const ModelView &view,
                 nxe::r2d::MeshChannel &out);

[[nodiscard]] usize masked_drawable_count(const ModelAsset &asset) noexcept;

} // namespace nxm::live2d
