#pragma once

#include "live2d/live2d_component.h"
#include "live2d/live2d_draw.h"
#include "live2d/live2d_pass.h"

#include "core/foundation/threading/thread_pool.h"

namespace nxe::r2d {
class MaterialSystem;
}

namespace nxe::scene {
class AssetRegistry;
}

namespace nxm::live2d {

inline constexpr nx::string_view SERVICE = "live2d.animation";

struct SceneView {
  u32 camera = 0;
  f32 depth_min = -1024.f;
  f32 depth_max = 1024.f;
  const nxe::r2d::MaterialSystem *materials = nullptr;
};

class Live2DSystem {
public:
  static void register_components(nxe::scene::registry_t &registry);

  usize load_pending(nxe::scene::registry_t &registry, f32 dt = 0.f);
  /// Reloads changed model bundles/subresources without dropping the last
  /// valid pose when a new generation is malformed.
  usize reload_changed(nxe::scene::registry_t &registry, bool force = false);

  usize update(nxe::scene::registry_t &registry,
               const nxe::scene::AssetRegistry &assets, f32 dt);
  usize update(nxe::scene::registry_t &registry, f32 dt);

  usize emit(nxe::scene::registry_t &registry, Frame &out,
             const SceneView &view);

  /// Caps transient mask memory per frame. A model beyond either budget draws
  /// unclipped; already accepted models remain correct.
  void set_mask_limits(u32 max_resolution, u64 bytes,
                       u32 atlas_count = 16) noexcept;

  usize on_low_memory(nxe::scene::registry_t &registry);

  [[nodiscard]] u32 mask_resolution_limit() const noexcept {
    return m_mask_resolution_limit;
  }
  [[nodiscard]] u64 mask_budget() const noexcept { return m_mask_budget; }
  [[nodiscard]] u32 mask_atlas_limit() const noexcept {
    return m_mask_atlas_limit;
  }

  void set_resolver(TextureResolver resolve) { m_resolve = std::move(resolve); }

  void set_threads(nx::thread_pool *const threads) noexcept {
    m_threads = threads;
  }

  using VoiceLevel = nx::function<f32(u32 voice)>;

  usize drive_lip_sync(nxe::scene::registry_t &registry,
                       const VoiceLevel &level);

private:
  /// One visible model's draws, built on its own before the frame takes them.
  struct EmitWork {
    Live2DRuntime *runtime = nullptr;
    ModelView view;
    bool wants_masks = false;
    Frame local;
    usize geometry = 0;
    usize masks = 0;
  };

  [[nodiscard]] u32 mask_resolution(u32 requested) const noexcept;

  TextureResolver m_resolve;
  nx::thread_pool *m_threads = nullptr;
  /// Reused between frames; only the first m_emit_count are this frame's.
  nx::vector<EmitWork> m_emit;
  usize m_emit_count = 0;
  u32 m_mask_resolution_limit = 2048;
  u32 m_mask_atlas_limit = 16;
  u64 m_mask_budget = u64{16} << 20;
  bool m_budget_warned = false;
};

} // namespace nxm::live2d
