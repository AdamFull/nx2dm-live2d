/**
 * @file test_live2d_scene.cpp
 * @brief A model on an entity, through the system, into a frame.
 */

#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/vfs/vfs.h"
#include "core/rendering/render2d/render_interop.h"
#include "live2d/live2d_system.h"

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;
namespace scene = nxe::scene;

constexpr u32 PAGE = 3;

struct World {
  bool ok = false;
  scene::registry_t registry;
  Live2DSystem system;

  World() {
    nx::vfs::initialize();
    nx::vfs::Device *const host =
        nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
    ok = host != nullptr && nx::vfs::mount("/", host);
    registry.register_component<scene::WorldTransform2D>(
        {.name = "WorldTransform2D"});
    Live2DSystem::register_components(registry);
    system.set_resolver(
        TextureResolver([](nx::string_view) { return pack_texture(PAGE, 0); }));
  }
  ~World() {
    registry.clear();
    nx::vfs::shutdown();
  }

  World(const World &) = delete;
  World &operator=(const World &) = delete;

  scene::Entity place(const nx::string_view path, const f32 scale = 1.f) {
    const scene::Entity e = registry.create();
    scene::WorldTransform2D &node =
        registry.emplace<scene::WorldTransform2D>(e);
    node.world[2][0] = 0.5f;
    node.world[2][1] = 0.5f;
    Live2DModel &model = registry.emplace<Live2DModel>(e);
    model.model = nx::string(path);
    model.scale = scale;
    return e;
  }
};

} // namespace

TEST_CASE("live2d: a model named by a component is loaded, posed and drawn") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity e = world.place(MODEL);
  // Nothing is loaded until the loading system runs, so a frame emitted before
  // it draws nothing rather than reaching into a component that has no moc.
  Frame early;
  CHECK(world.system.emit(world.registry, early, {}) == 0u);
  CHECK(early.empty());

  REQUIRE(world.system.load_pending(world.registry) == 1u);
  const Live2DRuntime *const runtime = world.registry.try_get<Live2DRuntime>(e);
  REQUIRE(runtime != nullptr);
  CHECK(runtime->ready());
  CHECK(runtime->loaded == MODEL);
  CHECK(runtime->masks.active());

  // Loading once. A second pass over an unchanged component reloads nothing,
  // which is what makes it safe to run every frame.
  CHECK(world.system.load_pending(world.registry) == 0u);

  CHECK(world.system.update(world.registry, 1.f / 60.f) == 1u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 1u);
  CHECK(!frame.empty());
  CHECK(frame.clips.size() == frame.geometry.draws.size());
  CHECK(!frame.masks.empty());
  CHECK(frame.atlas_size == runtime->masks.atlas_size());

  // The resolver's answer reached the draws rather than being dropped.
  for (const nxe::r2d::MeshDraw &draw : frame.geometry.draws)
    CHECK((draw.texture >> 16) == PAGE);
}

TEST_CASE("live2d: the component's placement and scale reach the vertices") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  world.place(MODEL, 1.f);
  REQUIRE(world.system.load_pending(world.registry) == 1u);
  Frame plain;
  REQUIRE(world.system.emit(world.registry, plain, {}) == 1u);

  World scaled;
  REQUIRE(scaled.ok);
  scaled.place(MODEL, 4.f);
  REQUIRE(scaled.system.load_pending(scaled.registry) == 1u);
  Frame big;
  REQUIRE(scaled.system.emit(scaled.registry, big, {}) == 1u);

  REQUIRE(plain.geometry.vertices.size() == big.geometry.vertices.size());
  // Scale is folded into the node's transform, and the node puts the model at
  // (0.5, 0.5) - so a vertex four times as far from that point.
  for (usize i = 0; i < plain.geometry.vertices.size(); ++i) {
    const glm::vec2 a = plain.geometry.vertices[i].position - glm::vec2(0.5f);
    const glm::vec2 b = big.geometry.vertices[i].position - glm::vec2(0.5f);
    CHECK(std::fabs(b.x - a.x * 4.f) < 1e-3f);
    CHECK(std::fabs(b.y - a.y * 4.f) < 1e-3f);
  }
}

TEST_CASE("live2d: an invisible model is stepped but not drawn") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity e = world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 1u);
  world.registry.get<Live2DModel>(e).visible = false;

  // Still animated: a model hidden for a moment should not jump when it comes
  // back, which it would if its clock stopped.
  CHECK(world.system.update(world.registry, 1.f / 60.f) == 1u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 0u);
  CHECK(frame.empty());
}

TEST_CASE("live2d: a component naming nothing, or a file that is not there") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity empty = world.registry.create();
  world.registry.emplace<scene::WorldTransform2D>(empty);
  world.registry.emplace<Live2DModel>(empty);

  const scene::Entity missing = world.place("/nothing/here.model3.json");

  // The empty one is not a request to load anything; the missing one is a
  // request that cannot be met.
  CHECK(world.system.load_pending(world.registry) == 0u);
  CHECK(world.registry.try_get<Live2DRuntime>(empty) == nullptr);

  const Live2DRuntime *const failed =
      world.registry.try_get<Live2DRuntime>(missing);
  REQUIRE(failed != nullptr);
  CHECK_FALSE(failed->ready());
  // Remembered as attempted, so the next frame does not try again. A missing
  // file will not appear, and retrying turns one typo into a log per frame.
  CHECK(failed->loaded == "/nothing/here.model3.json");
  CHECK(world.system.load_pending(world.registry) == 0u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 0u);
}

TEST_CASE("live2d: a voice opens the mouth, and letting go closes it") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity e = world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 1u);
  Live2DModel &model = world.registry.get<Live2DModel>(e);

  // No voice, no lip sync - a component driving its mouth by hand should not
  // have it overwritten.
  model.mouth = 0.4f;
  CHECK(world.system.drive_lip_sync(
            world.registry,
            Live2DSystem::VoiceLevel([](u32) { return 1.f; })) == 0u);
  CHECK(model.mouth == 0.4f);

  // A quarter loud, gain three, so three quarters open.
  model.voice = 17;
  CHECK(world.system.drive_lip_sync(world.registry,
                                    Live2DSystem::VoiceLevel([](const u32 v) {
                                      CHECK(v == 17u);
                                      return 0.25f;
                                    })) == 1u);
  CHECK(std::fabs(model.mouth - 0.75f) < 1e-5f);

  // Loud enough to clip, rather than driving the parameter past its range.
  CHECK(world.system.drive_lip_sync(
            world.registry,
            Live2DSystem::VoiceLevel([](u32) { return 0.9f; })) == 1u);
  CHECK(model.mouth == 1.f);

  // The line ends. The mouth closes and the handle goes, so a later voice
  // landing on the same slot does not start driving this model.
  CHECK(world.system.drive_lip_sync(
            world.registry,
            Live2DSystem::VoiceLevel([](u32) { return -1.f; })) == 0u);
  CHECK(model.mouth == 0.f);
  CHECK(model.voice == 0u);
}

TEST_CASE("live2d: only one model in a frame gets the mask atlas") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  world.place(MODEL);
  world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 2u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 2u);

  // Both drew, and the clips stayed one per draw - the second model's are all
  // unclipped because every layout packs as though it owned the whole atlas.
  // Wrong in the way L3 was, and loudly, rather than two models overwriting
  // each other's tiles.
  CHECK(frame.clips.size() == frame.geometry.draws.size());
  usize clipped = 0;
  for (const DrawMask &clip : frame.clips)
    clipped += clip.clipped() ? 1u : 0u;
  CHECK(clipped > 0u);
  CHECK(clipped < frame.geometry.draws.size());
}
