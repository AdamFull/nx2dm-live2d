
#include "core/app/engine.h"
#include "core/app/script_runtime.h"
#include "core/script/luau/luau_backend.h"
#include "core/script/luau/luau_runtime.h"

#include "live2d/live2d_component.h"

#include "core/foundation/diagnostics/log.h"

namespace {

void configure(nxe::EngineConfig &config) {
  config.app.name = "{{project}}";
  config.app.window.title = "{{project}}";
  config.app.window.width = 1280;
  config.app.window.height = 720;

  config.action_map = {};
  config.saved_bindings = {};
  config.ui_styles = {};
  config.physics_rules = {};
  config.audio_bank = {};
}

void add_model(nxe::Engine &engine) {
    const nxe::scene::Entity e = engine.scene().create_node("model");
    engine.scene().set_position(e, {0.f, -2.4f});

    nxm::live2d::Live2DModel model;
    model.model = "/live2d/Hiyori.model3.json";
    model.scale = 2.4f;
    model.motion = "Idle";
    model.motion_index = 0;
    model.motion_loop = true;
    model.blink = true;
    model.breathe = true;
    engine.scene().registry().emplace<nxm::live2d::Live2DModel>(e, std::move(model));
}

bool on_create(nxe::Engine &engine) {
  const nxe::scene::Entity camera = engine.scene().create_node("camera");
  engine.scene().registry().emplace<nxe::scene::Camera2D>(
      camera, nxe::scene::Camera2D{.ortho_height = 6.f, .active = true});
  engine.scene().set_active_camera(camera);

  add_model(engine);
  if (nxe::start_scripts(
          engine, {.backend = nxe::script::luau_backend(),
                   .expose_game = {},
                   .load_module = &nxe::script::load_luau_module}))
    (void)engine.scripts().define(engine.schedule(), "model", "game.model");

  nx::logi("{{project}}: up");
  return true;
}

}

NX_IMPLEMENT_GAME(.configure = configure, .on_create = on_create)
