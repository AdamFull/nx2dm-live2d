#pragma once

#include "live2d/live2d_assets.h"

namespace nxm::live2d {

/// How long a change of expression weight takes to go all the way.
inline constexpr f32 EXPRESSION_WEIGHT_SECONDS = 0.5f;

/// How quickly the model turns to where it is asked to look: the time for
/// about two thirds of the way.
inline constexpr f32 LOOK_SECONDS = 0.15f;

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
  /// How strongly the expression shows, from 0 (not at all) to 1, eased
  /// there over @p ease seconds of updates.
  void set_expression_weight(f32 weight,
                             f32 ease = EXPRESSION_WEIGHT_SECONDS) noexcept;
  [[nodiscard]] f32 expression_weight() const noexcept {
    return m_expression_weight;
  }

  void update(f32 dt);

  bool set_parameter(nx::string_view id, f32 value);
  [[nodiscard]] f32 parameter(nx::string_view id) const;

  void refresh();

  void set_blinking(bool on) noexcept { m_blinking = on; }
  [[nodiscard]] bool blinking() const noexcept { return m_blinking; }

  void set_breathing(bool on) noexcept { m_breathing = on; }
  [[nodiscard]] bool breathing() const noexcept { return m_breathing; }

  void set_mouth(f32 amount) noexcept { m_mouth = nx::clamp(amount, 0.f, 1.f); }
  /// Where the model looks, from -1 to 1 across and up, 0 straight ahead;
  /// it turns its head, body and eyes there over LOOK_SECONDS. @p eased
  /// false turns it there at once.
  void set_look(f32 x, f32 y, bool eased = true) noexcept;
  [[nodiscard]] f32 look_x() const noexcept { return m_look_x; }
  [[nodiscard]] f32 look_y() const noexcept { return m_look_y; }
  [[nodiscard]] f32 mouth() const noexcept { return m_mouth; }

  [[nodiscard]] bool motion_finished() const noexcept;
  [[nodiscard]] f32 elapsed() const noexcept { return m_elapsed; }
  [[nodiscard]] nx::string_view motion_group() const noexcept {
    return m_motion_group.view();
  }
  [[nodiscard]] i32 motion_index() const noexcept { return m_motion_index; }
  [[nodiscard]] bool motion_loop() const noexcept { return m_motion_loop; }
  [[nodiscard]] nx::string_view expression() const noexcept {
    return m_expression.view();
  }

private:
  ModelAsset *m_asset = nullptr;
  nx::string m_expression;
  f32 m_expression_weight = 1.f;
  f32 m_expression_target = 1.f;
  f32 m_expression_ease = 0.f;
  /// The parameters as they were before the expression, while it shows at
  /// less than full weight.
  nx::vector<f32> m_unexpressed;
  nx::string m_motion_group;
  i32 m_motion_index = 0;
  bool m_motion_loop = false;
  f32 m_elapsed = 0.f;
  f32 m_mouth = 0.f;
  f32 m_look_x = 0.f;
  f32 m_look_y = 0.f;
  f32 m_look_target_x = 0.f;
  f32 m_look_target_y = 0.f;
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
