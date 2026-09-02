#include "live2d/live2d_system.h"

#include "core/foundation/diagnostics/log.h"
#include "core/rendering/render2d/material_system.h"
#include "core/scene/animation_graph.h"
#include "core/scene/assets.h"
#include "core/foundation/vfs/vfs.h"

#include <cmath>
#include <limits>

namespace nxm::live2d {
namespace {

namespace scene = nxe::scene;

constexpr f32 LOAD_RETRY_BASE_SECONDS = 1.f;
constexpr f32 LOAD_RETRY_MAX_SECONDS = 30.f;
constexpr u32 MIN_MASK_RESOLUTION = 64;
constexpr u32 MAX_MASK_ATLASES = 16;
constexpr u64 MASK_TEXEL_BYTES = 4;

[[nodiscard]] u64 source_stamp(
    const std::span<const nx::string> dependencies) noexcept {
  return nx::vfs::files_generation(dependencies);
}

struct MotionRef {
  nx::string_view group;
  i32 index = 0;
};

/// A graph slot names a motion group and may select an entry as `Group#2`.
/// Without a suffix the component's motion_index remains the project default.
[[nodiscard]] MotionRef motion_ref(const nx::string_view slot,
                                   const i32 fallback) noexcept {
  const usize hash = slot.find_last_of('#');
  if (hash == nx::string_view::npos || hash == 0 || hash + 1 >= slot.size())
    return {slot, fallback};
  i64 index = 0;
  for (usize i = hash + 1; i < slot.size(); ++i) {
    const char digit = slot[i];
    if (digit < '0' || digit > '9')
      return {slot, fallback};
    index = index * 10 + (digit - '0');
    if (index > std::numeric_limits<i32>::max())
      return {slot, fallback};
  }
  return {slot.substr(0, hash), nx::cast<i32>(index)};
}

}

void Live2DSystem::register_components(scene::registry_t &registry) {
  registry.register_component<Live2DModel>({.name = "Live2DModel"});
  registry.register_component<Live2DRuntime>({.name = "Live2DRuntime"});
}

u32 Live2DSystem::mask_resolution(const u32 requested) const noexcept {
  return nx::clamp(requested, MIN_MASK_RESOLUTION, m_mask_resolution_limit);
}

void Live2DSystem::set_mask_limits(const u32 max_resolution, const u64 bytes,
                                   const u32 atlas_count) noexcept {
  m_mask_resolution_limit =
      nx::clamp(max_resolution, MIN_MASK_RESOLUTION, 16384u);
  m_mask_atlas_limit = nx::clamp(atlas_count, 1u, MAX_MASK_ATLASES);
  constexpr u64 smallest = u64{MIN_MASK_RESOLUTION} * MIN_MASK_RESOLUTION *
                           MASK_TEXEL_BYTES;
  m_mask_budget = nx::max(bytes, smallest);
  m_budget_warned = false;
}

usize Live2DSystem::load_pending(scene::registry_t &registry, const f32 dt) {
  usize loaded = 0;
  nx::vector<scene::Entity> wanted;
  const f32 elapsed = std::isfinite(dt) ? nx::max(dt, 0.f) : 0.f;

  registry.view<Live2DModel>().each(
      [&](const scene::Entity e, const Live2DModel &model) {
        if (model.model.empty()) {
          (void)registry.remove<Live2DRuntime>(e);
          return;
        }
        Live2DRuntime *const runtime = registry.try_get<Live2DRuntime>(e);
        if (runtime == nullptr || runtime->requested != model.model) {
          wanted.push_back(e);
          return;
        }
        if (runtime->ready() && runtime->loaded == model.model)
          return;
        runtime->retry_in = nx::max(runtime->retry_in - elapsed, 0.f);
        if (runtime->retry_in == 0.f)
          wanted.push_back(e);
      });

  for (const scene::Entity e : wanted) {
    const Live2DModel &model = registry.get<Live2DModel>(e);
    Live2DRuntime *runtime = registry.try_get<Live2DRuntime>(e);
    if (runtime == nullptr || runtime->requested != model.model)
      runtime = registry.try_emplace_or_replace<Live2DRuntime>(
          e, Live2DRuntime{});
    runtime->requested = model.model;
    runtime->animator.bind(nullptr);
    runtime->masks.clear();
    runtime->asset = ModelAsset{};

    nx::string error;
    if (!load_model(model.model.view(), m_resolve, runtime->asset, error)) {
      nx::loge("live2d: {}", error);
      runtime->loaded.clear();
      const u32 exponent = nx::min(runtime->load_failures, 5u);
      runtime->retry_in = nx::min(
          LOAD_RETRY_BASE_SECONDS * nx::cast<f32>(u32{1} << exponent),
          LOAD_RETRY_MAX_SECONDS);
      runtime->load_failures = nx::min(runtime->load_failures + 1u, 32u);
      continue;
    }
    runtime->loaded = model.model;
    runtime->retry_in = 0.f;
    runtime->load_failures = 0;
    runtime->animator.bind(&runtime->asset);
    if (!model.motion.empty() || model.motion_index != 0)
      (void)runtime->animator.play(model.motion.view(), model.motion_index,
                                  model.motion_loop);
    runtime->animator.update(0.f);
    (void)runtime->masks.build(runtime->asset,
                               mask_resolution(model.mask_resolution), 1);
    runtime->masks.update(runtime->asset);
    runtime->source_stamp = source_stamp(runtime->asset.dependencies());
    ++loaded;
  }
  return loaded;
}

usize Live2DSystem::reload_changed(scene::registry_t &registry) {
  usize loaded = 0;
  registry.view<const Live2DModel, Live2DRuntime>().each(
      [&](const scene::Entity, const Live2DModel &model,
          Live2DRuntime &runtime) {
        if (!runtime.ready() || runtime.loaded != model.model)
          return;
        const u64 changed = source_stamp(runtime.asset.dependencies());
        if (changed == runtime.source_stamp)
          return;
        // Observe this failed generation once. A subsequent editor save has a
        // different stamp and retries; the current runtime remains untouched.
        runtime.source_stamp = changed;

        ModelAsset fresh_asset;
        nx::string error;
        if (!load_model(model.model.view(), m_resolve, fresh_asset, error)) {
          nx::logw("live2d: '{}' changed but its last valid generation remains: "
                   "{}",
                   model.model, error);
          return;
        }

        const nx::string motion(runtime.animator.motion_group());
        const i32 motion_index = runtime.animator.motion_index();
        const bool motion_loop = runtime.animator.motion_loop();
        const nx::string expression(runtime.animator.expression());
        const f32 elapsed = runtime.animator.elapsed();
        const bool blinking = runtime.animator.blinking();
        const bool breathing = runtime.animator.breathing();
        const f32 mouth = runtime.animator.mouth();

        Live2DRuntime fresh;
        fresh.asset = std::move(fresh_asset);
        fresh.requested = model.model;
        fresh.loaded = model.model;
        fresh.animator.bind(&fresh.asset);
        fresh.animator.set_blinking(blinking);
        fresh.animator.set_breathing(breathing);
        fresh.animator.set_mouth(mouth);
        if (!motion.empty())
          (void)fresh.animator.play(motion.view(), motion_index, motion_loop,
                                    0.f);
        if (!expression.empty())
          (void)fresh.animator.set_expression(expression.view());
        fresh.animator.update(elapsed);
        (void)fresh.masks.build(fresh.asset,
                                mask_resolution(model.mask_resolution), 1);
        fresh.masks.update(fresh.asset);
        fresh.source_stamp = source_stamp(fresh.asset.dependencies());
        runtime = std::move(fresh);
        ++loaded;
        nx::logi("live2d: reloaded '{}'", model.model);
      });
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

usize Live2DSystem::update(scene::registry_t &registry,
                           const scene::AssetRegistry &assets, const f32 dt) {
  usize stepped = 0;
  registry.view<Live2DModel, Live2DRuntime>().each([&](const scene::Entity e,
                                                       const Live2DModel &model,
                                                       Live2DRuntime &runtime) {
    if (!runtime.ready() || runtime.loaded != model.model)
      return;
    f32 animation_speed = 1.f;
    if (auto *controller =
            registry.try_get<scene::AnimationGraphComponent>(e);
        controller != nullptr && !controller->clip_set.valid()) {
      const scene::AnimationGraph *const graph = assets.graph(controller->graph);
      if (graph != nullptr) {
        const auto duration = [&](const u32 slot, const u16, f32 &seconds) {
          const MotionRef motion =
              motion_ref(graph->clip_slot_name(slot), model.motion_index);
          return runtime.animator.motion_duration(motion.group, motion.index,
                                                  seconds);
        };
        scene::GraphTick tick;
        if (scene::update_animation_state_machine(
                *controller, *graph, duration, dt * model.time_scale, tick)) {
          const scene::AnimationState *const state =
              graph->state(controller->state);
          if (state != nullptr) {
            animation_speed = state->speed * controller->speed;
            if (tick.entered != scene::INVALID_STATE) {
              const MotionRef motion = motion_ref(
                  graph->clip_slot_name(state->clip_slot), model.motion_index);
              (void)runtime.animator.play(
                  motion.group, motion.index,
                  state->mode == scene::PlayMode::Loop,
                  controller->blending() ? controller->blend_duration : 0.f);
            }
          }
          if (!controller->playing)
            animation_speed = 0.f;
        }
      }
    }
    runtime.animator.set_blinking(model.blink);
    runtime.animator.set_breathing(model.breathe);
    runtime.animator.set_mouth(model.mouth);
    runtime.animator.update(dt * model.time_scale * animation_speed);
    runtime.masks.update(runtime.asset);
    ++stepped;
  });
  return stepped;
}

usize Live2DSystem::update(scene::registry_t &registry, const f32 dt) {
  static const scene::AssetRegistry no_graph_assets;
  return update(registry, no_graph_assets, dt);
}

usize Live2DSystem::emit(scene::registry_t &registry, Frame &out,
                         const SceneView &view) {
  static const MaskLayout NONE;
  u64 mask_bytes = 0;
  for (const u32 size : out.atlas_sizes)
    mask_bytes += nx::cast<u64>(size) * size * MASK_TEXEL_BYTES;
  usize over_budget = 0;

  usize drawn = 0;
  registry.view<Live2DModel, Live2DRuntime, scene::WorldTransform2D>().each(
      [&](const scene::Entity, const Live2DModel &model, Live2DRuntime &runtime,
          const scene::WorldTransform2D &node) {
        if (!model.visible || !runtime.ready() ||
            runtime.loaded != model.model)
          return;

        ModelView emit_view;
        emit_view.world = node.world;
        emit_view.world[0] *= model.scale;
        emit_view.world[1] *= model.scale;
        emit_view.color = model.color;
        emit_view.layer = model.layer;
        emit_view.camera = view.camera;
        emit_view.depth_min = view.depth_min;
        emit_view.depth_max = view.depth_max;
        if (view.materials != nullptr && model.material != 0u) {
          emit_view.batch = view.materials->batch_of(model.material);
          emit_view.material = view.materials->offset_of(model.material);
        }

        const u32 size = runtime.masks.atlas_size();
        const u32 count = runtime.masks.atlas_count();
        const u64 per_atlas = nx::cast<u64>(size) * size * MASK_TEXEL_BYTES;
        const u64 required = per_atlas * count;
        const bool wants_masks = runtime.masks.active();
        const usize held_atlases = out.atlas_sizes.size();
        const bool has_atlas_room =
            held_atlases <= m_mask_atlas_limit &&
            count <= m_mask_atlas_limit - nx::cast<u32>(held_atlases);
        const bool has_byte_room =
            mask_bytes <= m_mask_budget && required <= m_mask_budget - mask_bytes;
        const bool can_mask = wants_masks && has_atlas_room && has_byte_room;
        if (wants_masks && !can_mask)
          ++over_budget;

        const usize model_vertices = out.geometry.vertices.size();
        const usize model_indices = out.geometry.indices.size();
        const usize model_draws = out.geometry.draws.size();
        const usize model_clips = out.clips.size();
        const usize mask_vertices = out.masks.vertices.size();
        const usize mask_indices = out.masks.indices.size();
        const usize mask_draws = out.masks.draws.size();
        const usize atlas_count = out.atlas_sizes.size();

        u32 atlas_base = 0;
        if (can_mask) {
          atlas_base = nx::cast<u32>(out.atlas_sizes.size());
          out.atlas_sizes.insert(out.atlas_sizes.end(), count, size);
          if (emit_masks(runtime.asset, runtime.masks, out.masks, atlas_base) ==
              0u) {
            out.atlas_sizes.resize(atlas_count);
            out.masks.vertices.resize(mask_vertices);
            out.masks.indices.resize(mask_indices);
            out.masks.draws.resize(mask_draws);
            atlas_base = 0;
          }
        }

        const bool mask_emitted = out.atlas_sizes.size() > atlas_count;
        const usize appended = emit_model(
            runtime.asset, emit_view, mask_emitted ? runtime.masks : NONE,
            out.geometry, out.clips, atlas_base);
        if (appended == 0) {
          out.geometry.vertices.resize(model_vertices);
          out.geometry.indices.resize(model_indices);
          out.geometry.draws.resize(model_draws);
          out.clips.resize(model_clips);
          out.masks.vertices.resize(mask_vertices);
          out.masks.indices.resize(mask_indices);
          out.masks.draws.resize(mask_draws);
          out.atlas_sizes.resize(atlas_count);
          return;
        }
        if (mask_emitted)
          mask_bytes += required;
        ++drawn;
      });
  if (over_budget > 0 && !m_budget_warned) {
    nx::logw("live2d: {} masked model(s) exceeded the frame's {} atlas / {} "
             "KiB mask budget and draw unclipped",
             over_budget, m_mask_atlas_limit, m_mask_budget >> 10);
    m_budget_warned = true;
  } else if (over_budget == 0) {
    m_budget_warned = false;
  }
  return drawn;
}

usize Live2DSystem::on_low_memory(scene::registry_t &registry) {
  u32 largest = 0;
  registry.view<Live2DModel, Live2DRuntime>().each(
      [&](const scene::Entity, const Live2DModel &model,
          const Live2DRuntime &runtime) {
        if (runtime.ready() && runtime.loaded == model.model &&
            runtime.masks.active())
          largest = nx::max(largest, runtime.masks.atlas_size());
      });

  const u32 relevant = largest != 0
                           ? nx::min(m_mask_resolution_limit, largest)
                           : m_mask_resolution_limit;
  m_mask_resolution_limit =
      nx::max(MIN_MASK_RESOLUTION, relevant / 2u);
  constexpr u64 smallest = u64{MIN_MASK_RESOLUTION} * MIN_MASK_RESOLUTION *
                           MASK_TEXEL_BYTES;
  m_mask_budget = nx::max(m_mask_budget / 2u, smallest);
  m_mask_atlas_limit = nx::max(m_mask_atlas_limit / 2u, 1u);

  usize reduced = 0;
  registry.view<Live2DModel, Live2DRuntime>().each(
      [&](const scene::Entity, const Live2DModel &model,
          Live2DRuntime &runtime) {
        if (!runtime.ready() || runtime.loaded != model.model ||
            !runtime.masks.active())
          return;
        const u32 resolution = mask_resolution(model.mask_resolution);
        if (runtime.masks.atlas_size() <= resolution)
          return;
        if (runtime.masks.build(runtime.asset, resolution, 1)) {
          runtime.masks.update(runtime.asset);
          ++reduced;
        }
      });
  m_budget_warned = false;
  return reduced;
}

}
