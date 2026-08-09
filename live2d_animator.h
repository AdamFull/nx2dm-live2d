#pragma once

/**
 * @file live2d_animator.h
 * @brief Driving one model through a frame (namespace nxm::live2d).
 */

#include "live2d/live2d_assets.h"

namespace nxm::live2d {

class Animator {
public:
  Animator() = default;
  explicit Animator(ModelAsset &asset) noexcept : m_asset(&asset) {}

  void bind(ModelAsset *asset) noexcept { m_asset = asset; }
  [[nodiscard]] ModelAsset *asset() const noexcept { return m_asset; }

  bool play(nx::string_view group, i32 index, bool loop = false);

  bool set_expression(nx::string_view name);

  void update(f32 dt);

  bool set_parameter(nx::string_view id, f32 value);
  [[nodiscard]] f32 parameter(nx::string_view id) const;

  void refresh();

  [[nodiscard]] bool motion_finished() const noexcept;
  [[nodiscard]] f32 elapsed() const noexcept { return m_elapsed; }
  [[nodiscard]] nx::string_view expression() const noexcept {
    return m_expression.view();
  }

private:
  ModelAsset *m_asset = nullptr;
  nx::string m_expression;
  f32 m_elapsed = 0.f;
};

/// Axis-aligned bounds of every visible drawable, in model units.
struct Bounds {
  f32 min_x = 0.f;
  f32 min_y = 0.f;
  f32 max_x = 0.f;
  f32 max_y = 0.f;

  [[nodiscard]] bool valid() const noexcept {
    return max_x > min_x && max_y > min_y;
  }
};

[[nodiscard]] Bounds visible_bounds(const ModelAsset &asset) noexcept;

void read_vertices(const ModelAsset &asset, nx::vector<f32> &out);

void read_parameters(const ModelAsset &asset, nx::vector<f32> &out);

[[nodiscard]] f32 uv_agreement(const ModelAsset &asset) noexcept;

} // namespace nxm::live2d
