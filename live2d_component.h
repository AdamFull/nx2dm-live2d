#pragma once

/**
 * @file live2d_component.h
 * @brief A model placed in a scene, and the pose it holds
 * (namespace nxm::live2d).
 */

#include "core/scene/components.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_mask.h"

#include <glm/vec4.hpp>

namespace nxm::live2d {

/// What a scene authors. Everything here survives a save and a load; nothing
/// here is a pointer into something loaded.
struct Live2DModel {
  nx::string model;
  glm::vec4 color{1.f, 1.f, 1.f, 1.f};
  f32 scale = 1.f;
  f32 time_scale = 1.f;
  nx::string motion;
  i32 motion_index = 0;
  bool motion_loop = true;
  i32 layer = 0;
  u32 mask_resolution = 512;
  bool visible = true;
  bool blink = true;
  bool breathe = true;
  f32 mouth = 0.f;
};

struct Live2DRuntime {
  ModelAsset asset;
  Animator animator;
  MaskLayout masks;
  nx::string loaded;

  Live2DRuntime() = default;
  ~Live2DRuntime() = default;

  Live2DRuntime(const Live2DRuntime &) = delete;
  Live2DRuntime &operator=(const Live2DRuntime &) = delete;

  Live2DRuntime(Live2DRuntime &&other) noexcept
      : asset(std::move(other.asset)), masks(std::move(other.masks)),
        loaded(std::move(other.loaded)) {
    animator = std::move(other.animator);
    animator.bind(&asset);
    other.animator.bind(nullptr);
  }

  Live2DRuntime &operator=(Live2DRuntime &&other) noexcept {
    if (this != &other) {
      asset = std::move(other.asset);
      masks = std::move(other.masks);
      loaded = std::move(other.loaded);
      animator = std::move(other.animator);
      animator.bind(&asset);
      other.animator.bind(nullptr);
    }
    return *this;
  }

  [[nodiscard]] bool ready() const noexcept { return asset.valid(); }
};

} // namespace nxm::live2d
