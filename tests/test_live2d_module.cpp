
#include "framework/nxtest.h"

#include "engine/stub_platform.h"

#include "app/engine.h"
#include "app/module_system/module.h"
#include "live2d/live2d_component.h"

namespace {

using namespace nxe;
using namespace nxm::live2d;

void quiet_configure(EngineConfig &config) {
  config.calibrate = false;
  config.action_map = {};
  config.physics_rules = {};
  config.audio = false;
}

struct Harness {
  test::StubPlatform platform;
  std::unique_ptr<Engine> engine;
  bool ready = false;

  Harness() {
    engine = std::make_unique<Engine>(Game{.configure = quiet_configure});
    for (const ModuleFactory factory : enabled_module_factories())
      engine->add_module(factory());
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

}

TEST_CASE("live2d: the module is in the build's registry") {
  bool found = false;
  for (const ModuleFactory factory : enabled_module_factories()) {
    const std::unique_ptr<Module> module = factory();
    found = found || (module != nullptr && module->name() == "live2d");
  }
  CHECK(found);
}

TEST_CASE("live2d: attaching wires the systems, the pass and the slot") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  CHECK(h.engine->scene().registry().is_component_registered<Live2DModel>());
  CHECK(h.engine->scene().registry().is_component_registered<Live2DRuntime>());

  const nx::vector<nx::string> unplaced = h.engine->schedule().unplaced();
  for (const nx::string_view name :
       {"live2d.load", "live2d.update", "live2d.emit"}) {
    CHECK(h.engine->schedule().defined(name));
    bool forgotten = false;
    for (const nx::string &idle : unplaced)
      forgotten = forgotten || idle == name;
    CHECK_FALSE(forgotten);
  }

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

TEST_CASE("live2d: its emit runs beside the other declared Present systems") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  // A stand-in for another module's draw system: it declared what it
  // touches, and that is none of Live2D's components.
  sys::Schedule &schedule = h.engine->schedule();
  REQUIRE(schedule.try_define("test.present",
                              sys::SystemFn([](const sys::Context &) {})));
  schedule.add(sys::Stage::Present, "test.present");
  schedule.declare<const scene::WorldTransform2D>("test.present");

  const auto systems = schedule.systems_in(sys::Stage::Present);
  const nx::vector<nx::vector<u32>> needs =
      schedule.dependencies(sys::Stage::Present);
  REQUIRE(needs.size() == systems.size());

  u32 emit = ~0u;
  for (u32 i = 0; i < systems.size(); ++i)
    if (systems.data()[i] == "live2d.emit")
      emit = i;
  REQUIRE(emit != ~0u);
  CHECK(schedule.access_of("live2d.emit").size() > 0u);

  const auto waits = [&](const u32 system, const u32 on) {
    for (const u32 before : needs[system])
      if (before == on)
        return true;
    return false;
  };
  usize beside = 0;
  for (u32 i = 0; i < systems.size(); ++i) {
    if (i == emit || schedule.access_of(systems.data()[i]).size() == 0u)
      continue;
    CHECK_FALSE(waits(i, emit));
    CHECK_FALSE(waits(emit, i));
    ++beside;
  }
  CHECK(beside > 0u);
}

TEST_CASE("live2d: the component reaches the scene format table") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  bool described = false;
  for (const scene::ComponentIO &io : h.engine->scene().formats().entries())
    described = described || io.node_key == "live2d";
  CHECK(described);
}

TEST_CASE("live2d: a frame with no model costs the module nothing") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  h.platform.advance(1.0 / 60.0);
  h.engine->on_tick(h.platform, 1.0 / 60.0);
  CHECK(h.engine->frame_index() == 1u);
}
