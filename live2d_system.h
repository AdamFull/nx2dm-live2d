#pragma once

#include "live2d/live2d_component.h"
#include "live2d/live2d_draw.h"
#include "live2d/live2d_pass.h"

#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/vfs/asset_pipeline.h"
#include "scene/animation/animation_graph.h"

namespace nxe::r2d {
class MaterialSystem;
}

namespace nxe::scene {
class AssetRegistry;
}

namespace nxm::live2d {

inline constexpr nx::string_view SERVICE = "live2d.animation";

struct GatheredModel {
  ModelSource source;
  nx::string error;
  bool ok = false;
};

struct PreparedModel {
  ModelAsset asset;
  nx::string error;
  bool ok = false;
};

using ModelLoadPipeline =
    nx::vfs::StagedAssetPipeline<GatheredModel, PreparedModel>;

struct SceneView {
  u32 camera = 0;
  f32 depth_min = -1024.f;
  f32 depth_max = 1024.f;
  const nxe::r2d::MaterialSystem *materials = nullptr;
  // Screen pixels per world unit along each axis; zero when unknown, which
  // renders masks at the resolution each model asks for.
  glm::vec2 pixels_per_unit{0.f};
};

// The mask atlas edge a model needs to match its size on screen: the next power
// of two of its larger on-screen extent, within [64, the layout's own].
[[nodiscard]] u32 mask_texels(const MaskLayout &masks, const CanvasInfo &canvas,
                              const glm::mat3 &world,
                              glm::vec2 pixels_per_unit) noexcept;

class Live2DSystem {
public:
  static void register_components(nxe::scene::registry_t &registry);

  usize load_pending(nxe::scene::registry_t &registry, f32 dt = 0.f);
  /// Reloads changed model bundles/subresources without dropping the last
  /// valid pose when a new generation is malformed.
  usize reload_changed(nxe::scene::registry_t &registry);
  /// Resolves every loaded model's texture pages again, for pages that have
  /// arrived since it loaded. Returns how many models changed.
  usize refresh_textures(nxe::scene::registry_t &registry);

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

  /// Mocs revived for models now loaded, one per file however many models
  /// share it.
  [[nodiscard]] usize shared_mocs() const noexcept { return m_mocs.size(); }
  /// Lets go of the mocs, before the Cubism framework shuts down.
  void release_mocs() noexcept { m_mocs.clear(); }

  /// Reads models on @p io and builds them on @p workers from now on, to be
  /// taken up by load_pending once ready; until then load_pending loads on the
  /// calling thread.
  void bind_loads(nx::vfs::AsyncIoService &io, nx::thread_pool &workers);
  /// Drains the loads still in flight, before the Cubism framework shuts
  /// down.
  void shutdown_loads() noexcept;
  [[nodiscard]] usize loads_in_flight() const noexcept {
    return m_pending.size();
  }

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
  /// One ready model to advance. Each owns its Cubism model, so models step in
  /// parallel.
  struct UpdateWork {
    const Live2DModel *model = nullptr;
    Live2DRuntime *runtime = nullptr;
    nxe::scene::AnimationGraphComponent *controller = nullptr;
  };

  void step(const UpdateWork &work, const nxe::scene::AssetRegistry &assets,
            f32 dt) const;

  /// One visible model's draws, built on its own before the frame takes them.
  struct EmitWork {
    Live2DRuntime *runtime = nullptr;
    ModelView view;
    bool wants_masks = false;
    u32 mask_size = 0;
    u32 positions = 0;
    nx::vector<ModelDraw> draws;
    nx::vector<MaskShape> masks;
  };

  [[nodiscard]] u32 mask_resolution(u32 requested) const noexcept;

  struct PendingLoad {
    nxe::scene::Entity entity{};
    nx::string path;
    nx::vfs::AssetLoadHandle handle;
  };

  void start_load(nxe::scene::Entity entity, const Live2DModel &model);
  usize take_loads(nxe::scene::registry_t &registry);
  void finish_load(const Live2DModel &model, Live2DRuntime &runtime);
  void fail_load(Live2DRuntime &runtime, const nx::string &error);

  TextureResolver m_resolve;
  MocCache m_mocs;
  // After the cache it builds into, so it drains first.
  ModelLoadPipeline m_loads;
  bool m_async = false;
  u64 m_load_generation = 0;
  nx::vector<PendingLoad> m_pending;
  nx::thread_pool *m_threads = nullptr;
  nx::vector<UpdateWork> m_updates;
  /// Reused between frames; only the first m_emit_count are this frame's.
  nx::vector<EmitWork> m_emit;
  usize m_emit_count = 0;
  u32 m_mask_resolution_limit = 2048;
  u32 m_mask_atlas_limit = 16;
  u64 m_mask_budget = u64{16} << 20;
  bool m_budget_warned = false;
};

} // namespace nxm::live2d
