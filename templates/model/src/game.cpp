/**
 * @file game.cpp
 * @brief {{project}}.
 */

#include "core/app/engine.h"

#include "live2d/live2d_component.h"
#include "core/app/script_services.h"
#include "core/script/luau/luau_backend.h"
#include "core/script/luau/luau_bindings.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/vfs.h"

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
    start_scripts(engine);

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

  /// The script host: a backend, the engine's services, and the prelude.
  ///
  /// assets/scripts/host.luau declares every service by name and shape, and
  /// the check below runs *both* ways - a service exposed and not declared is
  /// as much an error as one declared and not exposed. Expose something of
  /// your own and you add a line there in the same change.
  void start_scripts(nxe::Engine &engine) {
    if (!engine.scripts().set_backend(nxe::script::luau_backend())) {
      nx::logw("{{project}}: no script backend; scripts will not run");
      return;
    }

    nxe::script::expose_core_services(engine.scripts());
    nxe::expose_action_services(engine.scripts(), engine.actions());
    nxe::expose_audio_services(engine.scripts(), engine.audio());
    nxe::expose_mixer_services(engine.scripts(), engine.mixer());
    nxe::expose_scene_services(engine.scripts(), engine.scene());
    nxe::expose_render_services(engine.scripts(), engine.render_vars());
    nxe::expose_ui_services(engine.scripts(), engine.ui());
    engine.scripts().expose_as("quit", [&engine] { engine.request_quit(); });

    // Modules a project's own code exposes go in before this: binding is
    // final, and a service offered afterwards is refused.
    if (!engine.scripts().bind()) {
      nx::logw("{{project}}: the VM took no host services");
      return;
    }

    for (const nx::string &name : engine.frame().modules())
      if (!load_script(engine, name))
        return;

    // The name the schedule places, and the key assets/scripts/model.luau
    // returns it under.
    (void)engine.scripts().define(engine.schedule(), "model", "game.model");
  }

  [[nodiscard]] static bool load_script(nxe::Engine &engine,
                                        const nx::string_view name) {
    const nx::string path = nx::format("/scripts/{}.luau", name);
    const auto text = nx::vfs::read_text(path);
    if (!text) {
      nx::loge("{{project}}: no {}", path);
      return false;
    }

    if (name == "host") {
      const auto generated = nx::vfs::read_text("/scripts/host_modules.luau");
      if (!generated) {
        nx::loge("{{project}}: no /scripts/host_modules.luau; build the "
                 "nx_host_declarations target");
        return false;
      }
      const nx::string_view sources[] = {text.value().view(),
                                         generated.value().view()};
      nx::string disagreement;
      if (!nxe::script::luau_host_types_agree(
              sources, engine.scripts().services(), disagreement)) {
        nx::loge("{{project}}: {} does not match the exposed services: {}",
                 path, disagreement);
        return false;
      }
    }

    return engine.scripts().load(
        name,
        {reinterpret_cast<const std::byte *>(text.value().data()),
         text.value().size()},
        path);
  }

};

} // namespace

NX_IMPLEMENT_GAME({{Project}}Game)
