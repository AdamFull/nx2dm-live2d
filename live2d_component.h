#pragma once

#include "core/scene/components.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_mask.h"

#include <glm/vec4.hpp>

namespace nxm::live2d {

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
  u32 voice = 0;
  f32 lip_sync_gain = 3.f;
  u32 material = 0;
};

struct Live2DRuntime {
  ModelAsset asset;
  Animator animator;
  MaskLayout masks;
  nx::string requested;
  nx::string loaded;
  f32 retry_in = 0.f;
  u32 load_failures = 0;

  Live2DRuntime() = default;
  ~Live2DRuntime() = default;

  Live2DRuntime(const Live2DRuntime &) = delete;
  Live2DRuntime &operator=(const Live2DRuntime &) = delete;

  Live2DRuntime(Live2DRuntime &&other) noexcept
      : asset(std::move(other.asset)), masks(std::move(other.masks)),
        requested(std::move(other.requested)), loaded(std::move(other.loaded)),
        retry_in(other.retry_in), load_failures(other.load_failures) {
    animator = std::move(other.animator);
    animator.bind(&asset);
    other.animator.bind(nullptr);
  }

  Live2DRuntime &operator=(Live2DRuntime &&other) noexcept {
    if (this != &other) {
      asset = std::move(other.asset);
      masks = std::move(other.masks);
      requested = std::move(other.requested);
      loaded = std::move(other.loaded);
      retry_in = other.retry_in;
      load_failures = other.load_failures;
      animator = std::move(other.animator);
      animator.bind(&asset);
      other.animator.bind(nullptr);
    }
    return *this;
  }

  [[nodiscard]] bool ready() const noexcept { return asset.valid(); }
};

}
