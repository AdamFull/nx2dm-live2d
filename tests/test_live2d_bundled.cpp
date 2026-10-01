
#include "framework/nxtest.h"

#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_assets.h"
#include "live2d/live2d_draw.h"
#include "live2d/live2d_system.h"

#include <glm/common.hpp>

namespace {

using namespace nxm::live2d;

constexpr nx::string_view MODEL = "/live2d/Hiyori.model3.json";

struct Mounted {
  bool ok = false;

  Mounted() {
    nx::vfs::initialize();
    nx::vfs::Device *const host = nx::vfs::make_host_device(
        nx::fs::path_view(nx::string_view(NX_LIVE2D_BUNDLED_DIR)));
    ok = host != nullptr && nx::vfs::mount("/", host);
  }
  ~Mounted() { nx::vfs::shutdown(); }

  Mounted(const Mounted &) = delete;
  Mounted &operator=(const Mounted &) = delete;
};

[[nodiscard]] TextureResolver counting(nx::vector<nx::string> &seen) {
  return TextureResolver([&seen](const nx::string_view path) {
    seen.push_back(nx::string(path));
    return nx::cast<u32>(seen.size()) << 16;
  });
}

[[nodiscard]] bool have_bundled() {
  return nx::fs::exists(nx::fs::path_view(
      nx::string(nx::string_view(NX_LIVE2D_BUNDLED_DIR)) + nx::string(MODEL)));
}

}

#if NX_REQUIRE_MODULE_FIXTURES
#define REQUIRE_BUNDLED()                                                      \
  do {                                                                         \
    if (!have_bundled())                                                       \
      FAIL("bundled Hiyori model absent but NX_REQUIRE_MODULE_FIXTURES set");  \
  } while (false)
#else
#define REQUIRE_BUNDLED()                                                      \
  do {                                                                         \
    if (!have_bundled())                                                       \
      SKIP("bundled Hiyori model absent from the build tree");                 \
  } while (false)
#endif

TEST_CASE("live2d: the bundled model loads with a moc, canvas and motions") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  nx::vector<nx::string> pages;
  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, counting(pages), asset, error));
  CHECK(error.empty());
  REQUIRE(asset.valid());

  CHECK(asset.parameter_count() > 0u);
  CHECK(asset.part_count() > 0u);
  CHECK(asset.drawable_count() > 0u);

  REQUIRE(!pages.empty());
  REQUIRE(asset.textures().size() == pages.size());
  for (usize i = 0; i < pages.size(); ++i)
    CHECK((asset.textures()[i] >> 16) == nx::cast<u32>(i + 1));

  const CanvasInfo canvas = asset.canvas();
  CHECK(canvas.width > 0.f);
  CHECK(canvas.height > 0.f);
  CHECK(canvas.pixels_per_unit > 0.f);

  CHECK(asset.has_physics());
  CHECK(!asset.motions().empty());
}

TEST_CASE("live2d: a view draws only the models on its layers") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);
  namespace scene = nxe::scene;
  scene::registry_t registry;
  registry.register_component<scene::WorldTransform2D>(
      {.name = "WorldTransform2D"});
  registry.register_component<scene::RenderLayers>({.name = "RenderLayers"});
  Live2DSystem::register_components(registry);
  Live2DSystem system;
  system.set_resolver(
      TextureResolver([](nx::string_view) { return pack_texture(1, 0); }));
  const scene::Entity e = registry.create();
  registry.emplace<scene::WorldTransform2D>(e);
  registry.emplace<Live2DModel>(e).model = nx::string(MODEL);
  REQUIRE(system.load_pending(registry) == 1u);

  // The window's view, by default, and a model on no layer of its own.
  Frame window;
  CHECK(system.emit(registry, window, {}) == 1u);

  registry.emplace<scene::RenderLayers>(e, scene::RenderLayers{.mask = 2u});
  Frame hidden;
  CHECK(system.emit(registry, hidden, {}) == 0u);
  CHECK(hidden.models.empty());
  Frame preview;
  CHECK(system.emit(registry, preview, {.camera = 3, .layers = 2u}) == 1u);
  REQUIRE(preview.models.size() == 1u);
  CHECK(preview.models[0].camera == 3u);
  // On both, both see it.
  registry.get<scene::RenderLayers>(e).mask = 2u | scene::WINDOW_LAYER;
  Frame both;
  CHECK(system.emit(registry, both, {}) == 1u);
  registry.clear();
}

TEST_CASE("live2d: models of one file share its moc and mesh, not their pose") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  MocCache mocs;
  nx::string error;
  ModelAsset first;
  ModelAsset second;
  REQUIRE(load_model(MODEL, {}, first, error, &mocs));
  REQUIRE(load_model(MODEL, {}, second, error, &mocs));
  CHECK(mocs.size() == 1u);
  REQUIRE(first.moc());
  CHECK(first.moc().get() == second.moc().get());
  CHECK(first.mesh().get() == second.mesh().get());
  CHECK(first.model() != second.model());

  ModelAsset alone;
  REQUIRE(load_model(MODEL, {}, alone, error));
  CHECK(alone.moc().get() != first.moc().get());
  CHECK(alone.mesh()->vertex_count() == first.mesh()->vertex_count());

  // Each is still its own instance: turning one leaves the other.
  Animator turned(first);
  Animator still(second);
  REQUIRE(turned.set_parameter("ParamAngleX", 30.f));
  turned.refresh();
  still.refresh();
  CHECK(turned.parameter("ParamAngleX") == 30.f);
  CHECK(still.parameter("ParamAngleX") == 0.f);
  nx::vector<f32> a, b;
  read_vertices(first, a);
  read_vertices(second, b);
  CHECK(a != b);
}

TEST_CASE("live2d: blinking does not undo an expression's eyes") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(
      load_model("/live2d/Hiyori.expression.model3.json", {}, asset, error));
  REQUIRE(asset.has_eye_blink());
  Animator animator(asset);
  animator.set_blinking(true);
  REQUIRE(animator.set_expression("EyesShut"));

  for (u32 i = 0; i < 30; ++i)
    animator.update(1.f / 60.f);
  // Long enough for several blinks, each of which reopens the eyes unless
  // the expression is applied over it.
  f32 widest = 0.f;
  for (u32 i = 0; i < 600; ++i) {
    animator.update(1.f / 60.f);
    widest = nx::max(widest, animator.parameter("ParamEyeLOpen"));
  }
  CHECK(widest < 0.05f);
}

TEST_CASE("live2d: an expression's weight sets how far it moves the model") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(
      load_model("/live2d/Hiyori.expression.model3.json", {}, asset, error));
  Animator animator(asset);
  const auto settle = [&](const f32 seconds) {
    for (f32 at = 0.f; at < seconds; at += 1.f / 60.f)
      animator.update(1.f / 60.f);
    return animator.parameter("ParamEyeLOpen");
  };
  const f32 open = settle(0.5f);
  REQUIRE(open > 0.9f);

  REQUIRE(animator.set_expression("EyesShut"));
  CHECK(settle(1.f) < 0.05f);

  animator.set_expression_weight(0.5f, 0.f);
  CHECK(settle(0.1f) == nxtest::Approx(open * 0.5f).margin(0.05f));
  animator.set_expression_weight(0.f, 0.f);
  CHECK(settle(0.1f) == nxtest::Approx(open).margin(0.02f));

  // Eased: part of the way after a moment, all of it after the whole time.
  animator.set_expression_weight(1.f);
  const f32 easing = settle(EXPRESSION_WEIGHT_SECONDS * 0.5f);
  CHECK(easing > 0.25f);
  CHECK(easing < 0.75f);
  CHECK(settle(EXPRESSION_WEIGHT_SECONDS) < 0.05f);
  CHECK(animator.expression_weight() == 1.f);
}

TEST_CASE("live2d: the model turns its head, body and eyes to where it is "
          "asked to look") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));
  Animator animator(asset);
  const auto settle = [&](const f32 seconds) {
    for (f32 at = 0.f; at < seconds; at += 1.f / 60.f)
      animator.update(1.f / 60.f);
  };
  settle(0.5f);
  CHECK(animator.parameter("ParamAngleX") == nxtest::Approx(0.f).margin(0.01f));

  animator.set_look(1.f, 0.f);
  settle(1.f / 60.f);
  // On its way, not there yet.
  const f32 turning = animator.parameter("ParamAngleX");
  CHECK(turning > 1.f);
  CHECK(turning < 10.f);
  settle(1.f);
  CHECK(animator.parameter("ParamAngleX") == nxtest::Approx(30.f).margin(0.5f));
  CHECK(animator.parameter("ParamBodyAngleX") ==
        nxtest::Approx(10.f).margin(0.2f));
  CHECK(animator.parameter("ParamEyeBallX") ==
        nxtest::Approx(1.f).margin(0.02f));
  CHECK(animator.parameter("ParamAngleY") == nxtest::Approx(0.f).margin(0.01f));

  // Out of range is the edge; at once is at once.
  animator.set_look(0.f, -4.f, false);
  settle(1.f / 60.f);
  CHECK(animator.parameter("ParamAngleY") == nxtest::Approx(-30.f));
  CHECK(animator.parameter("ParamAngleX") == nxtest::Approx(0.f).margin(0.01f));
  CHECK(animator.parameter("ParamEyeBallY") == nxtest::Approx(-1.f));

  animator.set_look(0.f, 0.f);
  settle(1.5f);
  CHECK(animator.parameter("ParamAngleY") == nxtest::Approx(0.f).margin(0.05f));
}

TEST_CASE("live2d: a shared moc outlives each model and goes with the last") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  MocCache mocs;
  nx::string error;
  ModelAsset first;
  ModelAsset second;
  REQUIRE(load_model(MODEL, {}, first, error, &mocs));
  REQUIRE(load_model(MODEL, {}, second, error, &mocs));

  first = ModelAsset();
  CHECK(mocs.prune() == 0u);
  CHECK(mocs.size() == 1u);
  // The survivor still poses from the moc the first one let go of.
  Animator animator(second);
  animator.update(1.f / 60.f);
  nx::vector<f32> vertices;
  read_vertices(second, vertices);
  CHECK(!vertices.empty());

  second = ModelAsset();
  CHECK(mocs.prune() == 1u);
  CHECK(mocs.size() == 0u);

  // Loading again revives it afresh.
  REQUIRE(load_model(MODEL, {}, first, error, &mocs));
  CHECK(mocs.size() == 1u);
}

TEST_CASE("live2d: the first moc added for a generation is the one kept") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  nx::string error;
  ModelAsset first;
  ModelAsset second;
  REQUIRE(load_model(MODEL, {}, first, error));
  REQUIRE(load_model(MODEL, {}, second, error));
  REQUIRE(first.moc().get() != second.moc().get());

  // Two loads that both missed the cache: the later one takes the earlier moc.
  MocCache mocs;
  CHECK(mocs.add("/m.moc3", 1u, first.moc()).get() == first.moc().get());
  CHECK(mocs.add("/m.moc3", 1u, second.moc()).get() == first.moc().get());
  CHECK(mocs.find("/m.moc3", 1u).get() == first.moc().get());
  CHECK(mocs.add("/m.moc3", 2u, second.moc()).get() == second.moc().get());
  CHECK(mocs.size() == 1u);
}

TEST_CASE("live2d: a moc of another generation is never reused") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  MocCache mocs;
  nx::string error;
  ModelAsset asset;
  REQUIRE(load_model(MODEL, {}, asset, error));
  const nx::shared_ptr<SharedMoc> moc = asset.moc();

  CHECK(mocs.add("/m.moc3", 1u, moc).get() == moc.get());
  CHECK(mocs.find("/m.moc3", 1u).get() == moc.get());
  CHECK_FALSE(mocs.find("/m.moc3", 2u));
  CHECK_FALSE(mocs.find("/other.moc3", 1u));
  // A new generation of the same file takes its place.
  (void)mocs.add("/m.moc3", 2u, moc);
  CHECK(mocs.size() == 1u);
  CHECK_FALSE(mocs.find("/m.moc3", 1u));
}

TEST_CASE("live2d: a hit area is where its drawable is in the pose") {
  REQUIRE_BUNDLED();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));
  REQUIRE(asset.hit_areas().size() == 1u);
  const HitArea &body = asset.hit_areas()[0];
  CHECK(body.name == "Body");
  CHECK(body.drawable == "HitArea");
  REQUIRE(body.index >= 0);
  Animator animator(asset);
  animator.update(1.f / 60.f);

  const DrawableMesh mesh = drawable_mesh(asset, body.index);
  REQUIRE_FALSE(mesh.positions.empty());
  glm::vec2 low = mesh.positions[0];
  glm::vec2 high = low;
  for (const glm::vec2 point : mesh.positions) {
    low = glm::min(low, point);
    high = glm::max(high, point);
  }
  const glm::vec2 middle = (low + high) * 0.5f;
  const auto inside = hit_areas_at(asset, middle);
  REQUIRE(inside.size() == 1u);
  CHECK(inside[0] == "Body");
  CHECK(hit_areas_at(asset, low).size() == 1u);
  CHECK(hit_areas_at(asset, high).size() == 1u);
  const glm::vec2 span = high - low;
  CHECK(hit_areas_at(asset, low - span * 0.01f).empty());
  CHECK(hit_areas_at(asset, {high.x + span.x * 0.01f, middle.y}).empty());
  CHECK(hit_areas_at(asset, {middle.x, high.y + span.y * 0.01f}).empty());
  CHECK(hit_areas_at(asset, {middle.x, low.y - span.y * 0.01f}).empty());
  CHECK(hit_areas_at(asset, {low.x - span.x * 0.01f, middle.y}).empty());

  // Where the model is drawn: moved, scaled, and back to model space.
  Live2DModel model;
  model.scale = 2.f;
  nxe::scene::WorldTransform2D node;
  node.world[2] = glm::vec3(3.f, -1.f, 1.f);
  // Off both axes, so neither one's scale can go unseen.
  const glm::vec2 point(0.25f, -0.4f);
  const std::optional<glm::vec2> local =
      model_point(model, node, {3.f + point.x * 2.f, -1.f + point.y * 2.f});
  REQUIRE(local.has_value());
  CHECK(local->x == nxtest::Approx(point.x).margin(1e-4f));
  CHECK(local->y == nxtest::Approx(point.y).margin(1e-4f));
  model.scale = 0.f;
  CHECK_FALSE(model_point(model, node, {0.f, 0.f}).has_value());
}
