#pragma once

/**
 * @file live2d_assets.h
 * @brief A model's shared data, loaded (namespace nxm::live2d).
 */

#include "core/foundation/core/callable.h"
#include "core/foundation/strings/utf8_string.h"

#include <span>

namespace Live2D::Cubism::Framework {
class CubismModel;
class CubismUserModel;
class ACubismMotion;
} // namespace Live2D::Cubism::Framework

namespace nxm::live2d {

using TextureResolver = nx::function<u32(nx::string_view path)>;

struct MotionEntry {
  nx::string group;
  i32 index = 0;
  Live2D::Cubism::Framework::ACubismMotion *motion = nullptr;
};

struct ExpressionEntry {
  nx::string name;
  Live2D::Cubism::Framework::ACubismMotion *motion = nullptr;
};

struct CanvasInfo {
  f32 width = 0.f;
  f32 height = 0.f;
  f32 origin_x = 0.f;
  f32 origin_y = 0.f;
  f32 pixels_per_unit = 1.f;
};

class ModelAsset {
public:
  ModelAsset() = default;
  ~ModelAsset();

  ModelAsset(const ModelAsset &) = delete;
  ModelAsset &operator=(const ModelAsset &) = delete;
  ModelAsset(ModelAsset &&other) noexcept;
  ModelAsset &operator=(ModelAsset &&other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return m_owner != nullptr; }

  [[nodiscard]] Live2D::Cubism::Framework::CubismUserModel *
  owner() const noexcept {
    return m_owner;
  }
  [[nodiscard]] Live2D::Cubism::Framework::CubismModel *model() const noexcept;

  [[nodiscard]] usize parameter_count() const noexcept;
  [[nodiscard]] usize part_count() const noexcept;
  [[nodiscard]] usize drawable_count() const noexcept;

  [[nodiscard]] CanvasInfo canvas() const noexcept { return m_canvas; }

  [[nodiscard]] std::span<const u32> textures() const noexcept {
    return {m_textures.data(), m_textures.size()};
  }

  [[nodiscard]] std::span<const MotionEntry> motions() const noexcept {
    return {m_motions.data(), m_motions.size()};
  }
  [[nodiscard]] std::span<const ExpressionEntry> expressions() const noexcept {
    return {m_expressions.data(), m_expressions.size()};
  }

  [[nodiscard]] Live2D::Cubism::Framework::ACubismMotion *
  find_motion(nx::string_view group, i32 index) const noexcept;
  [[nodiscard]] Live2D::Cubism::Framework::ACubismMotion *
  find_expression(nx::string_view name) const noexcept;

  [[nodiscard]] bool has_physics() const noexcept { return m_physics; }
  [[nodiscard]] bool has_pose() const noexcept { return m_pose; }

  [[nodiscard]] std::span<const nx::string> missing() const noexcept {
    return {m_missing.data(), m_missing.size()};
  }

private:
  friend bool load_model(nx::string_view, TextureResolver, ModelAsset &,
                         nx::string &);

  void reset() noexcept;

  Live2D::Cubism::Framework::CubismUserModel *m_owner = nullptr;
  nx::vector<u32> m_textures;
  nx::vector<MotionEntry> m_motions;
  nx::vector<ExpressionEntry> m_expressions;
  nx::vector<nx::string> m_missing;
  CanvasInfo m_canvas;
  bool m_physics = false;
  bool m_pose = false;
};

[[nodiscard]] bool load_model(nx::string_view model3_path,
                              TextureResolver resolve, ModelAsset &out,
                              nx::string &error);

} // namespace nxm::live2d
