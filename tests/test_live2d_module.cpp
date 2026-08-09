/**
 * @file test_live2d_module.cpp
 * @brief The module attaching to an Engine.
 *
 * The forty lines that wire the systems, the pass, the slot, the scene format
 * and the script bindings were the last part of this module with nothing
 * covering them - every piece they wire was tested and the wiring itself was
 * not. Cheap to get wrong, too: a system defined and never added to a stage
 * runs silently never, and so does a pass that fills no slot.
 */

#include "framework/nxtest.h"

#include "engine/stub_platform.h"

#include "core/app/engine.h"
#include "core/app/module.h"
#include "live2d/live2d_component.h"

namespace {

using namespace nxe;
using namespace nxm::live2d;

/// Asks for nothing, so what the engine ends up with came from the module.
class QuietGame final : public IGame {
public:
  void configure(EngineConfig &config) override {
    config.calibrate = false;
    config.action_map = {};
    config.physics_rules = {};
    config.audio = false;
  }
};

/// The engine with the module added the way NX_IMPLEMENT_GAME adds it.
struct Harness {
  test::StubPlatform platform;
  std::unique_ptr<Engine> engine;
  bool ready = false;

  Harness() {
    engine = std::make_unique<Engine>(std::make_unique<QuietGame>());
    for (Module *const module : enabled_modules())
      engine->add_module(module);
    // The build's own output directory, so load_shader finds the module's
    // shader where a game would find it.
    platform.set_assets_path(NX_TEST_RUNTIME_DIR);
    rt::AppConfig config;
    engine->configure(config);
    ready = engine->on_create(platform);
  }
  ~Harness() {
    if (engine && ready)
      engine->on_destroy();
  }

  Harness(const Harness &) = delete;
  Harness &operator=(const Harness &) = delete;
};

[[nodiscard]] bool ordered(const Engine &engine, const nx::string_view name) {
  for (const nx::string &pass : engine.passes().ordered())
    if (pass == name)
      return true;
  return false;
}

} // namespace

TEST_CASE("live2d: the module is in the build's registry") {
  bool found = false;
  for (const Module *const module : enabled_modules())
    found = found || module->name() == "live2d";
  CHECK(found);
}

TEST_CASE("live2d: attaching wires the systems, the pass and the slot") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  // Registered before the game runs, so a scene naming a Live2DModel loads.
  CHECK(h.engine->scene().registry().is_component_registered<Live2DModel>());
  CHECK(h.engine->scene().registry().is_component_registered<Live2DRuntime>());

  // Defined *and* placed in a stage. A system that is only defined never runs
  // - "nothing runs because it exists" - and that is the easier half to forget.
  const nx::vector<nx::string> unplaced = h.engine->schedule().unplaced();
  for (const nx::string_view name :
       {"live2d.load", "live2d.update", "live2d.emit"}) {
    CHECK(h.engine->schedule().defined(name));
    bool forgotten = false;
    for (const nx::string &idle : unplaced)
      forgotten = forgotten || idle == name;
    CHECK_FALSE(forgotten);
  }

  // The pass reached the frame through the `world` slot, in the right place:
  // after the scene target is opened and before the UI goes over it.
  CHECK(h.engine->passes().defined("live2d.draw"));
  REQUIRE(ordered(*h.engine, "live2d.draw"));

  usize scene = ~usize{0};
  usize model = ~usize{0};
  usize ui = ~usize{0};
  const std::span<const nx::string> order = h.engine->passes().ordered();
  for (usize i = 0; i < order.size(); ++i) {
    if (order[i] == "core.scene")
      scene = i;
    if (order[i] == "live2d.draw")
      model = i;
    if (order[i] == "core.ui")
      ui = i;
  }
  REQUIRE(scene != ~usize{0});
  CHECK(scene < model);
  if (ui != ~usize{0})
    CHECK(model < ui);
}

TEST_CASE("live2d: the component reaches the scene format table") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  // Without this a .nxscene carrying a model loads it as nothing at all, with
  // no error: an unknown key in a scene document is simply skipped.
  bool described = false;
  for (const scene::ComponentIO &io : h.engine->scene().formats().entries())
    described = described || io.node_key == "live2d";
  CHECK(described);
}

TEST_CASE("live2d: a frame with no model costs the module nothing") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  // The systems run every frame in every project that enables the module. A
  // scene with no Live2DModel in it should not notice.
  h.platform.advance(1.0 / 60.0);
  h.engine->on_tick(h.platform, 1.0 / 60.0);
  CHECK(h.engine->frame_index() == 1u);
}
