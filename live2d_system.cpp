#include "live2d/live2d_system.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::live2d {
namespace {

namespace scene = nxe::scene;

} // namespace

void Live2DSystem::register_components(scene::registry_t &registry) {
  registry.register_component<Live2DModel>({.name = "Live2DModel"});
  registry.register_component<Live2DRuntime>({.name = "Live2DRuntime"});
}

usize Live2DSystem::load_pending(scene::registry_t &registry) {
  usize loaded = 0;
  nx::vector<scene::Entity> wanted;

  registry.view<Live2DModel>().each(
      [&](const scene::Entity e, const Live2DModel &model) {
        if (model.model.empty())
          return;
        const Live2DRuntime *const runtime = registry.try_get<Live2DRuntime>(e);
        if (runtime == nullptr || runtime->loaded != model.model)
          wanted.push_back(e);
      });

  for (const scene::Entity e : wanted) {
    const Live2DModel &model = registry.get<Live2DModel>(e);
    Live2DRuntime &runtime =
        *registry.try_emplace_or_replace<Live2DRuntime>(e, Live2DRuntime{});

    nx::string error;
    if (!load_model(model.model.view(), m_resolve, runtime.asset, error)) {
      nx::loge("live2d: {}", error);
      runtime.loaded = model.model;
      continue;
    }
    runtime.loaded = model.model;
    runtime.animator.bind(&runtime.asset);
    (void)runtime.masks.build(runtime.asset,
                              nx::max(model.mask_resolution, 64u), 1);
    if (!model.motion.empty() || model.motion_index != 0)
      (void)runtime.animator.play(model.motion.view(), model.motion_index,
                                  model.motion_loop);
    runtime.animator.update(0.f);
    runtime.masks.update(runtime.asset);
    ++loaded;
  }
  return loaded;
}

usize Live2DSystem::drive_lip_sync(scene::registry_t &registry,
                                   const VoiceLevel &level) {
  usize speaking = 0;
  registry.view<Live2DModel>().each(
      [&](const scene::Entity, Live2DModel &model) {
        if (model.voice == 0 || !level)
          return;
        const f32 amplitude = level(model.voice);
        if (amplitude < 0.f) {
          model.voice = 0;
          model.mouth = 0.f;
          return;
        }
        model.mouth = nx::clamp(amplitude * model.lip_sync_gain, 0.f, 1.f);
        ++speaking;
      });
  return speaking;
}

usize Live2DSystem::update(scene::registry_t &registry, const f32 dt) {
  usize stepped = 0;
  registry.view<Live2DModel, Live2DRuntime>().each([&](const scene::Entity,
                                                       const Live2DModel &model,
                                                       Live2DRuntime &runtime) {
    if (!runtime.ready())
      return;
    runtime.animator.set_blinking(model.blink);
    runtime.animator.set_breathing(model.breathe);
    runtime.animator.set_mouth(model.mouth);
    runtime.animator.update(dt * model.time_scale);
    // After the pose and not before: a mask's tile is fitted to where the
    // art it clips has just moved to.
    runtime.masks.update(runtime.asset);
    ++stepped;
  });
  return stepped;
}

usize Live2DSystem::emit(scene::registry_t &registry, Frame &out,
                         const SceneView &view) {
  static const MaskLayout NONE;
  bool masked_taken = false;

  usize drawn = 0;
  registry.view<Live2DModel, Live2DRuntime, scene::WorldTransform2D>().each(
      [&](const scene::Entity, const Live2DModel &model, Live2DRuntime &runtime,
          const scene::WorldTransform2D &node) {
        if (!model.visible || !runtime.ready())
          return;

        ModelView emit_view;
        // The node's own transform with the component's scale folded in, so a
        // model sized in scene units does not need its own scale node.
        emit_view.world = node.world;
        emit_view.world[0] *= model.scale;
        emit_view.world[1] *= model.scale;
        emit_view.color = model.color;
        emit_view.layer = model.layer;
        emit_view.camera = view.camera;
        emit_view.depth_min = view.depth_min;
        emit_view.depth_max = view.depth_max;

        const bool wants_masks = runtime.masks.active();
        const bool mine = wants_masks && !masked_taken;
        if (wants_masks && !mine)
          nx::logw("live2d: a second masked model in one frame draws "
                   "unclipped; the atlas holds one model's masks");

        const usize appended =
            emit_model(runtime.asset, emit_view, mine ? runtime.masks : NONE,
                       out.geometry, out.clips);
        if (appended == 0)
          return;
        if (mine) {
          (void)emit_masks(runtime.asset, runtime.masks, out.masks);
          out.atlas_size = runtime.masks.atlas_size();
          masked_taken = true;
        }
        ++drawn;
      });
  return drawn;
}

} // namespace nxm::live2d
