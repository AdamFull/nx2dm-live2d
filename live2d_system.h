#pragma once

/**
 * @file live2d_system.h
 * @brief Posing a scene's models and drawing them (namespace nxm::live2d).
 */

#include "live2d/live2d_component.h"
#include "live2d/live2d_pass.h"

namespace nxm::live2d {

struct SceneView {
  u32 camera = 0;
  f32 depth_min = -1024.f;
  f32 depth_max = 1024.f;
};

class Live2DSystem {
public:
  static void register_components(nxe::scene::registry_t &registry);

  usize load_pending(nxe::scene::registry_t &registry, f32 dt = 0.f);

  usize update(nxe::scene::registry_t &registry, f32 dt);

  usize emit(nxe::scene::registry_t &registry, Frame &out,
             const SceneView &view);

  /// Caps transient mask memory per frame. A model beyond either budget draws
  /// unclipped; already accepted models remain correct.
  void set_mask_limits(u32 max_resolution, u64 bytes,
                       u32 atlas_count = 16) noexcept;

  /// Reduces mask resolution and future transient budgets, then rebuilds live
  /// layouts at the smaller size. Returns the layouts actually reduced.
  usize on_low_memory(nxe::scene::registry_t &registry);

  [[nodiscard]] u32 mask_resolution_limit() const noexcept {
    return m_mask_resolution_limit;
  }
  [[nodiscard]] u64 mask_budget() const noexcept { return m_mask_budget; }
  [[nodiscard]] u32 mask_atlas_limit() const noexcept {
    return m_mask_atlas_limit;
  }

  void set_resolver(TextureResolver resolve) { m_resolve = std::move(resolve); }

  using VoiceLevel = nx::function<f32(u32 voice)>;

  usize drive_lip_sync(nxe::scene::registry_t &registry,
                       const VoiceLevel &level);

private:
  [[nodiscard]] u32 mask_resolution(u32 requested) const noexcept;

  TextureResolver m_resolve;
  u32 m_mask_resolution_limit = 2048;
  u32 m_mask_atlas_limit = 16;
  u64 m_mask_budget = u64{16} << 20;
  bool m_budget_warned = false;
};

} // namespace nxm::live2d
