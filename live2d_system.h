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

  usize load_pending(nxe::scene::registry_t &registry);

  usize update(nxe::scene::registry_t &registry, f32 dt);

  usize emit(nxe::scene::registry_t &registry, Frame &out,
             const SceneView &view);

  void set_resolver(TextureResolver resolve) { m_resolve = std::move(resolve); }

private:
  TextureResolver m_resolve;
};

} // namespace nxm::live2d
