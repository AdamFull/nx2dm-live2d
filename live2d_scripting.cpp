#include "live2d/live2d_scripting.h"

#include "live2d/live2d_component.h"

#include "core/app/engine.h"
#include "core/script/script_host.h"

namespace nxm::live2d {
namespace {

namespace scene = nxe::scene;
namespace sys = nxe::sys;

[[nodiscard]] Live2DRuntime *runtime_of(nxe::ModuleContext &ctx,
                                        const sys::Entity e) {
  if (e == sys::Entity{})
    return nullptr;
  Live2DRuntime *const runtime =
      ctx.scene().registry().try_get<Live2DRuntime>(e);
  return runtime != nullptr && runtime->ready() ? runtime : nullptr;
}

[[nodiscard]] Live2DModel *model_of(nxe::ModuleContext &ctx,
                                    const sys::Entity e) {
  if (e == sys::Entity{})
    return nullptr;
  return ctx.scene().registry().try_get<Live2DModel>(e);
}

} // namespace

void expose_live2d_services(nxe::script::Host &host, nxe::ModuleContext &ctx) {
  host.expose_as("live2d_play", [&ctx](const sys::Entity e,
                                       const nx::string_view group,
                                       const f32 index, const bool loop) {
    Live2DRuntime *const runtime = runtime_of(ctx, e);
    return runtime != nullptr &&
           runtime->animator.play(group, nx::cast<i32>(index), loop);
  });

  host.expose_as("live2d_expression", [&ctx](const sys::Entity e,
                                             const nx::string_view name) {
    Live2DRuntime *const runtime = runtime_of(ctx, e);
    return runtime != nullptr && runtime->animator.set_expression(name);
  });

  host.expose_as("live2d_finished", [&ctx](const sys::Entity e) {
    const Live2DRuntime *const runtime = runtime_of(ctx, e);
    // No model is not "still playing": a script waiting on this would wait for
    // ever rather than move on.
    return runtime == nullptr || runtime->animator.motion_finished();
  });

  host.expose_as("live2d_set_param", [&ctx](const sys::Entity e,
                                            const nx::string_view id,
                                            const f32 value) {
    Live2DRuntime *const runtime = runtime_of(ctx, e);
    return runtime != nullptr && runtime->animator.set_parameter(id, value);
  });

  host.expose_as(
      "live2d_param", [&ctx](const sys::Entity e, const nx::string_view id) {
        const Live2DRuntime *const runtime = runtime_of(ctx, e);
        return runtime == nullptr ? 0.f : runtime->animator.parameter(id);
      });

  // On the component rather than the runtime: these survive a save, and a
  // model that has not loaded yet should still remember what it was told.
  host.expose_as("live2d_visible",
                 [&ctx](const sys::Entity e, const bool on) {
                   Live2DModel *const model = model_of(ctx, e);
                   if (model == nullptr)
                     return false;
                   model->visible = on;
                   return true;
                 });

  host.expose_as("live2d_mouth",
                 [&ctx](const sys::Entity e, const f32 open) {
                   Live2DModel *const model = model_of(ctx, e);
                   if (model == nullptr)
                     return false;
                   // Hand-driving the mouth means nothing else should: a voice
                   // still bound would overwrite this on the very next frame.
                   model->voice = 0;
                   model->mouth = nx::clamp(open, 0.f, 1.f);
                   return true;
                 });

  host.expose_as("live2d_speak",
                 [&ctx](const sys::Entity e, const nx::string_view event) {
                   Live2DModel *const model = model_of(ctx, e);
                   if (model == nullptr || event.empty())
                     return false;
                   const nxe::audio::VoiceHandle voice =
                       ctx.audio().play(nx::id_string(event));
                   if (!voice.valid())
                     return false;
                   model->voice = voice.raw();
                   return true;
                 });
}

usize drive_lip_sync(nxe::ModuleContext &ctx, Live2DSystem &system) {
  return system.drive_lip_sync(
      ctx.scene().registry(),
      Live2DSystem::VoiceLevel([&ctx](const u32 raw) {
        const auto voice = nxe::audio::VoiceHandle::from_raw(raw);
        return ctx.mixer().is_playing(voice) ? ctx.mixer().amplitude(voice)
                                             : -1.f;
      }));
}

} // namespace nxm::live2d
