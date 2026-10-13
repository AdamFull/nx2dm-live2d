
#include "framework/nxtest.h"

#include "engine/stub_platform.h"

#include "app/engine.h"
#include "app/module_system/module.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_component.h"

#include <chrono>
#include <optional>

namespace {

using namespace nxe;
using namespace nxm::live2d;

void quiet_configure(EngineConfig &config) {
  config.calibrate = false;
  config.action_map = {};
  config.physics_rules = {};
  config.audio = false;
  // Read back, so a test can see what a window drew.
  config.offscreen_render = true;
  config.swapchain_format = rhi::Format::RGBA8_UNORM;
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

/// @p window's next frame, once no pipeline has been compiling for a while:
/// they compile asynchronously, and a frame drawn before is missing draws.
[[nodiscard]] std::optional<FrameCapture> capture(Harness &h,
                                                  const rt::WindowId window) {
  u32 quiet = 0;
  for (u32 tick = 0; tick < 5000 && quiet < 30u; ++tick) {
    h.engine->on_tick(h.platform, 1.0 / 60.0);
    const GpuPipelineLoadStats loads = h.engine->gpu_pipeline_load_stats();
    quiet = loads.pending_pipelines == 0 && loads.pending_shader_sources == 0
                ? quiet + 1u
                : 0u;
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const u64 token = h.engine->request_frame_capture(window);
  for (u32 frame = 0; frame < 600 && token != 0; ++frame) {
    h.engine->on_tick(h.platform, 1.0 / 60.0);
    if (h.engine->frame_capture_status(token) == FrameCaptureStatus::Ready)
      return h.engine->take_frame_capture(token);
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

/// Pixels of @p image that are not the clear colour.
[[nodiscard]] usize drawn(const FrameCapture &image) {
  usize count = 0;
  for (usize i = 0; i + 3 < image.pixels.size(); i += 4)
    count += image.pixels[i] > 40u || image.pixels[i + 1] > 40u ||
                     image.pixels[i + 2] > 40u
                 ? 1u
                 : 0u;
  return count;
}

[[nodiscard]] bool ordered(const Engine &engine, const nx::string_view name) {
  for (const nx::string &pass : engine.passes().ordered())
    if (pass == name)
      return true;
  return false;
}

}

TEST_CASE("live2d: a scene made after the module knows its components and "
          "reads them from a scene file") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");
  const scene::SceneId id = h.engine->create_scene();
  REQUIRE(id.valid());
  scene::Scene &made = *h.engine->scene(id);
  CHECK(made.registry().component_metadata_named("Live2DModel") != nullptr);
  bool readable = false;
  for (const scene::ComponentIO &io : made.formats().entries())
    readable = readable || io.node_key == "live2d";
  CHECK(readable);
}

TEST_CASE("live2d: a preview scene's model loads while the game is paused, "
          "and a game scene's waits for it") {
  constexpr nx::string_view MODEL = "/live2d/Hiyori.model3.json";
  if (!nx::fs::exists(
          nx::fs::path_view(nx::string(nx::string_view(NX_LIVE2D_BUNDLED_DIR)) +
                            nx::string(MODEL))))
    SKIP("bundled Hiyori model absent from the build tree");
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");
  const nx::vfs::MountId bundled =
      nx::vfs::mount("/",
                     nx::vfs::make_host_device(nx::fs::path_view(
                         nx::string_view(NX_LIVE2D_BUNDLED_DIR))),
                     100);
  REQUIRE(bundled.valid());

  const auto shown = [&](const scene::SceneId id) {
    scene::Scene &held = *h.engine->scene(id);
    const scene::Entity e = held.create_node("model");
    held.registry().emplace<Live2DModel>(
        e, Live2DModel{.model = nx::string(MODEL)});
    return e;
  };
  const auto ready = [&](const scene::SceneId id, const scene::Entity e) {
    const Live2DRuntime *const runtime =
        h.engine->scene(id)->registry().try_get<Live2DRuntime>(e);
    return runtime != nullptr && runtime->ready();
  };
  const scene::SceneId game = h.engine->create_scene();
  const scene::SceneId preview = h.engine->create_preview_scene();
  const scene::Entity in_game = shown(game);
  const scene::Entity in_preview = shown(preview);

  h.engine->set_paused(true);
  for (u32 frame = 0; frame < 3000 && !ready(preview, in_preview); ++frame) {
    h.engine->on_tick(h.platform, 1.0 / 60.0);
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(ready(preview, in_preview));
  CHECK(h.engine->scene(game)->registry().try_get<Live2DRuntime>(in_game) ==
        nullptr);

  h.engine->set_paused(false);
  for (u32 frame = 0; frame < 3000 && !ready(game, in_game); ++frame) {
    h.engine->on_tick(h.platform, 1.0 / 60.0);
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(ready(game, in_game));
  // A scene that goes while its model loads leaves nothing behind.
  REQUIRE(h.engine->destroy_scene(preview));
  h.engine->on_tick(h.platform, 1.0 / 60.0);
  nx::vfs::unmount(bundled);
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

  // Into the textures cameras draw into, through the offscreen node they
  // bind, recorded before the UI shows them.
  CHECK(h.engine->passes().defined("live2d.targets"));
  usize offscreen = ~usize{0};
  for (usize i = 0; i < order.size(); ++i)
    if (order[i] == nxe::PASS_OFFSCREEN)
      offscreen = i;
  REQUIRE(offscreen != ~usize{0});
  if (ui != ~usize{0})
    CHECK(offscreen < ui);
  const std::span<const nx::string> cameras =
      h.engine->passes().offscreen_ordered("");
  bool targets = false;
  for (const nx::string &pass : cameras)
    targets = targets || pass == "live2d.targets";
  CHECK(targets);
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

TEST_CASE("live2d: its world and targets passes draw for every node") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  const nx::string_view twice[] = {"live2d.draw", "live2d.targets",
                                   "live2d.draw", "live2d.targets"};
  h.engine->passes().order(twice);
  CHECK(h.engine->passes().repeated().empty());
}

TEST_CASE("live2d: its passes draw only where there is a camera") {
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");

  const ComposedNode nodes[] = {{.name = "world"},
                                {.name = "post", .kind = FrameNodeKind::Group}};
  const FramePasses::OrderedPass order[] = {{"live2d.draw", 0},
                                            {"live2d.targets", 0},
                                            {"live2d.draw", 1},
                                            {"live2d.targets", 1}};
  h.engine->passes().order_from_frame(order, nodes);
  const nx::vector<nx::string> unmet = h.engine->passes().unmet();
  REQUIRE(unmet.size() == 2u);
  CHECK(unmet[0].view().starts_with("'live2d.draw' needs view, which node "
                                    "'post'"));
  CHECK(unmet[1].view().starts_with("'live2d.targets' needs view, which node "
                                    "'post'"));
}

TEST_CASE("live2d: a window layer bound to a scene draws its models") {
  constexpr nx::string_view MODEL = "/live2d/Hiyori.model3.json";
  if (!nx::fs::exists(
          nx::fs::path_view(nx::string(nx::string_view(NX_LIVE2D_BUNDLED_DIR)) +
                            nx::string(MODEL))))
    SKIP("bundled Hiyori model absent from the build tree");
  Harness h;
  if (!h.ready)
    SKIP("no usable RHI device");
  const nx::vfs::MountId bundled =
      nx::vfs::mount("/",
                     nx::vfs::make_host_device(nx::fs::path_view(
                         nx::string_view(NX_LIVE2D_BUNDLED_DIR))),
                     100);
  REQUIRE(bundled.valid());

  const scene::SceneId id = h.engine->create_preview_scene();
  scene::Scene &preview = *h.engine->scene(id);
  const scene::Entity e = preview.create_node("model");
  preview.registry().emplace<Live2DModel>(
      e, Live2DModel{.model = nx::string(MODEL)});
  for (u32 frame = 0; frame < 3000; ++frame) {
    const Live2DRuntime *const runtime =
        preview.registry().try_get<Live2DRuntime>(e);
    if (runtime != nullptr && runtime->ready())
      break;
    h.engine->on_tick(h.platform, 1.0 / 60.0);
    nx::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const rt::WindowId window =
      h.engine->open_window({.title = "preview", .width = 64, .height = 64});
  REQUIRE(window.valid());
  REQUIRE(h.engine->set_window_frame(window, WindowFrame::Preview));
  // Hiyori is a couple of units tall about the origin.
  REQUIRE(h.engine->bind_layer_view(window, PREVIEW_LAYER, id,
                                    {.eye = {0.f, 0.f}, .ortho_height = 4.f}));
  usize seen = 0;
  for (u32 attempt = 0; attempt < 10 && seen < 40u; ++attempt) {
    const std::optional<FrameCapture> image = capture(h, window);
    REQUIRE(image.has_value());
    seen = drawn(*image);
  }
  CHECK(seen >= 40u);
  h.engine->close_window(window);
  nx::vfs::unmount(bundled);
}
