#pragma once

#include "live2d/live2d_assets.h"

namespace nxm::live2d {

class Animator {
public:
  Animator() = default;
  explicit Animator(ModelAsset &asset) noexcept : m_asset(&asset) {}

  void bind(ModelAsset *asset) noexcept { m_asset = asset; }
  [[nodiscard]] ModelAsset *asset() const noexcept { return m_asset; }

  bool play(nx::string_view group, i32 index, bool loop = false,
            f32 fade_seconds = -1.f);
  [[nodiscard]] bool motion_duration(nx::string_view group, i32 index,
                                     f32 &duration) const;

  bool set_expression(nx::string_view name);

  void update(f32 dt);

  bool set_parameter(nx::string_view id, f32 value);
  [[nodiscard]] f32 parameter(nx::string_view id) const;

  void refresh();

  void set_blinking(bool on) noexcept { m_blinking = on; }
  [[nodiscard]] bool blinking() const noexcept { return m_blinking; }

  void set_breathing(bool on) noexcept { m_breathing = on; }
  [[nodiscard]] bool breathing() const noexcept { return m_breathing; }

  void set_mouth(f32 amount) noexcept { m_mouth = nx::clamp(amount, 0.f, 1.f); }
  [[nodiscard]] f32 mouth() const noexcept { return m_mouth; }

  [[nodiscard]] bool motion_finished() const noexcept;
  [[nodiscard]] f32 elapsed() const noexcept { return m_elapsed; }
  [[nodiscard]] nx::string_view expression() const noexcept {
    return m_expression.view();
  }

private:
  ModelAsset *m_asset = nullptr;
  nx::string m_expression;
  f32 m_elapsed = 0.f;
  f32 m_mouth = 0.f;
  bool m_blinking = false;
  bool m_breathing = false;
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

}
