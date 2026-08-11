/**
 * @file game.cpp
 * @brief {{project}}.
 */

#include "core/app/engine.h"
#include "core/app/script_runtime.h"
#include "core/script/luau/luau_backend.h"
#include "core/script/luau/luau_runtime.h"

#include "live2d/live2d_component.h"

#include "core/foundation/diagnostics/log.h"

namespace {

class {{Project}}Game final : public nxe::IGame {
public:
  void configure(nxe::EngineConfig &config) override {
    config.app.name = "{{project}}";
    config.app.window.title = "{{project}}";
    config.app.window.width = 1280;
    config.app.window.height = 720;

    // Empty rather than the engine's default paths: those name files a project
    // is expected to bring, and a scaffolded one has none yet. Each has a
    // built-in fallback - no bindings, the dark style, default physics rules.
    // Point them at your own once you have them.
    config.action_map = {};
    config.saved_bindings = {};
    config.ui_styles = {};
    config.physics_rules = {};
    config.audio_bank = {};
  }

  bool on_create(nxe::Engine &engine) override {
    // A camera, because a scene with none draws nothing and says nothing about
    // why. ortho_height is how much of the world fits top to bottom.
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

  /// The model, placed by hand rather than by a scene file: one node, one
  /// component, and the module's load system picks it up on the next frame.
  /// A .nxscene can carry the same thing under a "live2d" key.
  void add_model(nxe::Engine &engine) {
    const nxe::scene::Entity e = engine.scene().create_node("model");
    engine.scene().set_position(e, {0.f, -2.4f});

    nxm::live2d::Live2DModel model;
    model.model = "/live2d/Hiyori.model3.json";
    model.scale = 2.4f;
    // The group a motion lives in, by name. Cubism exports name them - Hiyori
    // has "Idle" and "TapBody" - and an empty name matches nothing at all,
    // which shows up only as a warning and a model that never moves.
    model.motion = "Idle";
    model.motion_index = 0;
    model.motion_loop = true;
    // Off by default in the runtime and on here: a character in a scene should
    // look alive without being asked, and a test wants nothing writing
    // parameters behind it.
    model.blink = true;
    model.breathe = true;
    engine.scene().registry().emplace<nxm::live2d::Live2DModel>(e, std::move(model));
  }

};

} // namespace

NX_IMPLEMENT_GAME({{Project}}Game)
