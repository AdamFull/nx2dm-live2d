
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_system.h"
#include "rendering/render2d/render_interop.h"
#include "scene/animation/animation_graph.h"
#include "scene/asset/assets.h"

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

}

TEST_CASE("live2d: mask limits retain a safe minimum under memory pressure") {
  scene::registry_t registry;
  Live2DSystem::register_components(registry);
  Live2DSystem system;
  CHECK(system.on_low_memory(registry) == 0u);
  CHECK(system.mask_resolution_limit() == 1024u);
  CHECK(system.mask_budget() == (u64{8} << 20));
  CHECK(system.mask_atlas_limit() == 8u);
  system.set_mask_limits(16384, 1, ~u32{0});
  CHECK(system.mask_atlas_limit() == 16u);
  system.set_mask_limits(1, 1, 0);
  CHECK(system.mask_resolution_limit() == 64u);
  CHECK(system.mask_budget() == u64{64} * 64 * 4);
  CHECK(system.mask_atlas_limit() == 1u);
  CHECK(system.on_low_memory(registry) == 0u);
  CHECK(system.mask_resolution_limit() == 64u);
  CHECK(system.mask_budget() == u64{64} * 64 * 4);
}

TEST_CASE("live2d: clearing a model request unloads its runtime") {
  scene::registry_t registry;
  Live2DSystem::register_components(registry);
  const scene::Entity e = registry.create();
  Live2DModel &model = registry.emplace<Live2DModel>(e);
  model.model = "/old/model3.json";
  registry.emplace<Live2DRuntime>(e).requested = model.model;

  model.model.clear();
  Live2DSystem system;
  CHECK(system.load_pending(registry) == 0u);
  CHECK(registry.try_get<Live2DRuntime>(e) == nullptr);
}

TEST_CASE("live2d: a failed model retries with exponential backoff") {
  nx::vfs::initialize();
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_memory_device()));

  scene::registry_t registry;
  Live2DSystem::register_components(registry);
  const scene::Entity e = registry.create();
  Live2DModel &model = registry.emplace<Live2DModel>(e);
  model.model = "/late/model3.json";
  Live2DSystem system;

  CHECK(system.load_pending(registry) == 0u);
  Live2DRuntime &runtime = registry.get<Live2DRuntime>(e);
  CHECK(runtime.requested == model.model);
  CHECK(runtime.loaded.empty());
  CHECK(runtime.load_failures == 1u);
  CHECK(runtime.retry_in == 1.f);

  CHECK(system.load_pending(registry) == 0u);
  CHECK(runtime.load_failures == 1u);
  CHECK(system.load_pending(registry, 1.f) == 0u);
  CHECK(runtime.load_failures == 2u);
  CHECK(runtime.retry_in == 2.f);

  registry.clear();
  nx::vfs::shutdown();
}

TEST_CASE("live2d: a model named by a component is loaded, posed and drawn") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity e = world.place(MODEL);
  Frame early;
  CHECK(world.system.emit(world.registry, early, {}) == 0u);
  CHECK(early.empty());

  REQUIRE(world.system.load_pending(world.registry) == 1u);
  const Live2DRuntime *const runtime = world.registry.try_get<Live2DRuntime>(e);
  REQUIRE(runtime != nullptr);
  CHECK(runtime->ready());
  CHECK(runtime->loaded == MODEL);
  CHECK(runtime->masks.active());

  CHECK(world.system.load_pending(world.registry) == 0u);

  CHECK(world.system.update(world.registry, 1.f / 60.f) == 1u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 1u);
  CHECK(!frame.empty());
  CHECK(frame.clips.size() == frame.geometry.draws.size());
  CHECK(!frame.masks.empty());
  REQUIRE(frame.atlas_sizes.size() == runtime->masks.atlas_count());
  CHECK(frame.atlas_sizes[0] == runtime->masks.atlas_size());

  for (const nxe::r2d::MeshDraw &draw : frame.geometry.draws)
    CHECK((draw.texture >> 16) == PAGE);
}

TEST_CASE("live2d: an engine graph transitions and fades native motions") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);
  world.registry.register_component<scene::AnimationGraphComponent>(
      {.name = "AnimationGraphComponent"});
  const scene::Entity entity = world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 1u);
  Live2DRuntime &runtime = world.registry.get<Live2DRuntime>(entity);
  REQUIRE(!runtime.asset.motions().empty());
  const MotionEntry &first = runtime.asset.motions()[0];
  const MotionEntry &second = runtime.asset.motions().size() > 1u
                                  ? runtime.asset.motions()[1]
                                  : runtime.asset.motions()[0];

  scene::AnimationGraph graph;
  graph.add_parameter("next", 0.f);
  const nx::string first_slot =
      nx::format("{}#{}", first.group, first.index);
  const nx::string second_slot =
      nx::format("{}#{}", second.group, second.index);
  const u16 first_state = nx::cast<u16>(
      graph.add_state("first", graph.add_clip_slot(first_slot.view())));
  const u16 second_state = nx::cast<u16>(
      graph.add_state("second", graph.add_clip_slot(second_slot.view())));
  const scene::TransitionCondition change[] = {
      {0, scene::Compare::Greater, 0.5f}};
  graph.add_transition(first_state, second_state, change, 0.2f);
  scene::AssetRegistry assets;
  const scene::GraphHandle graph_handle = assets.add_graph(std::move(graph));
  scene::AnimationGraphComponent controller;
  controller.graph = graph_handle;
  controller.clip_set = scene::INVALID_CLIP_SET;
  world.registry.emplace<scene::AnimationGraphComponent>(entity, controller);

  REQUIRE(world.system.update(world.registry, assets, 0.f) == 1u);
  CHECK(world.registry.get<scene::AnimationGraphComponent>(entity).state ==
        first_state);
  CHECK_FALSE(runtime.animator.motion_finished());

  world.registry.get<scene::AnimationGraphComponent>(entity).params[0] = 1.f;
  REQUIRE(world.system.update(world.registry, assets, 0.016f) == 1u);
  const scene::AnimationGraphComponent &transitioned =
      world.registry.get<scene::AnimationGraphComponent>(entity);
  CHECK(transitioned.state == second_state);
  CHECK(transitioned.blending());
  CHECK(transitioned.blend_duration == nxtest::Approx(0.2f));
  CHECK_FALSE(runtime.animator.motion_finished());
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

  CHECK(world.system.load_pending(world.registry) == 0u);
  CHECK(world.registry.try_get<Live2DRuntime>(empty) == nullptr);

  const Live2DRuntime *const failed =
      world.registry.try_get<Live2DRuntime>(missing);
  REQUIRE(failed != nullptr);
  CHECK_FALSE(failed->ready());
  CHECK(failed->loaded.empty());
  CHECK(failed->requested == "/nothing/here.model3.json");
  CHECK(failed->load_failures == 1u);
  const f32 retry = failed->retry_in;
  CHECK(retry > 0.f);
  CHECK(world.system.load_pending(world.registry) == 0u);
  CHECK(failed->load_failures == 1u);
  CHECK(world.system.load_pending(world.registry, retry) == 0u);
  CHECK(failed->load_failures == 2u);

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

  model.mouth = 0.4f;
  CHECK(world.system.drive_lip_sync(
            world.registry,
            Live2DSystem::VoiceLevel([](u32) { return 1.f; })) == 0u);
  CHECK(model.mouth == 0.4f);

  model.voice = 17;
  CHECK(world.system.drive_lip_sync(world.registry,
                                    Live2DSystem::VoiceLevel([](const u32 v) {
                                      CHECK(v == 17u);
                                      return 0.25f;
                                    })) == 1u);
  CHECK(std::fabs(model.mouth - 0.75f) < 1e-5f);

  CHECK(world.system.drive_lip_sync(
            world.registry,
            Live2DSystem::VoiceLevel([](u32) { return 0.9f; })) == 1u);
  CHECK(model.mouth == 1.f);

  CHECK(world.system.drive_lip_sync(
            world.registry,
            Live2DSystem::VoiceLevel([](u32) { return -1.f; })) == 0u);
  CHECK(model.mouth == 0.f);
  CHECK(model.voice == 0u);
}

TEST_CASE("live2d: every masked model in a frame gets its own atlas") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity first = world.place(MODEL);
  const scene::Entity second = world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 2u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 2u);

  CHECK(frame.clips.size() == frame.geometry.draws.size());
  REQUIRE(frame.atlas_sizes.size() == 2u);
  const Live2DRuntime &a = world.registry.get<Live2DRuntime>(first);
  const Live2DRuntime &b = world.registry.get<Live2DRuntime>(second);
  const usize expected =
      masked_drawable_count(a.asset) + masked_drawable_count(b.asset);
  usize clipped = 0;
  bool sampled_first = false;
  bool sampled_second = false;
  for (const DrawMask &clip : frame.clips)
    if (clip.clipped()) {
      ++clipped;
      sampled_first = sampled_first || clip.atlas == 0u;
      sampled_second = sampled_second || clip.atlas == 1u;
    }
  CHECK(clipped == expected);
  CHECK(sampled_first);
  CHECK(sampled_second);

  bool drew_first = false;
  bool drew_second = false;
  for (const MaskDraw &draw : frame.masks.draws) {
    drew_first = drew_first || draw.atlas == 0u;
    drew_second = drew_second || draw.atlas == 1u;
  }
  CHECK(drew_first);
  CHECK(drew_second);
}

TEST_CASE("live2d: the mask budget degrades excess models without collisions") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);
  constexpr u64 one_atlas = u64{512} * 512 * 4;
  world.system.set_mask_limits(512, one_atlas, 1);

  const scene::Entity first = world.place(MODEL);
  world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 2u);

  Frame frame;
  CHECK(world.system.emit(world.registry, frame, {}) == 2u);
  REQUIRE(frame.atlas_sizes.size() == 1u);
  const usize expected = masked_drawable_count(
      world.registry.get<Live2DRuntime>(first).asset);
  usize clipped = 0;
  for (const DrawMask &clip : frame.clips)
    clipped += clip.clipped() ? 1u : 0u;
  CHECK(clipped == expected);
  for (const MaskDraw &draw : frame.masks.draws)
    CHECK(draw.atlas == 0u);
}

TEST_CASE("live2d: models built on the pool emit what one thread does") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);
  // Room for two models' masks: the rest go over budget and draw unclipped.
  constexpr u64 two_atlases = u64{2} * 512 * 512 * 4;
  world.system.set_mask_limits(512, two_atlases, 2);
  for (u32 i = 0; i < 5; ++i) {
    const scene::Entity e = world.place(MODEL);
    world.registry.get<scene::WorldTransform2D>(e).world[2][0] =
        nx::cast<f32>(i) * 0.3f;
  }
  REQUIRE(world.system.load_pending(world.registry) == 5u);

  // Pooled first, so its per-model buffers start empty rather than holding
  // what a serial pass already built.
  nx::thread_pool pool;
  world.system.set_threads(&pool);
  Frame pooled;
  REQUIRE(world.system.emit(world.registry, pooled, {}) == 5u);
  world.system.set_threads(nullptr);
  Frame serial;
  REQUIRE(world.system.emit(world.registry, serial, {}) == 5u);

  REQUIRE(serial.atlas_sizes.size() == 2u);
  CHECK(pooled.atlas_sizes.size() == serial.atlas_sizes.size());
  REQUIRE(pooled.geometry.vertices.size() == serial.geometry.vertices.size());
  REQUIRE(pooled.geometry.indices.size() == serial.geometry.indices.size());
  REQUIRE(pooled.geometry.draws.size() == serial.geometry.draws.size());
  REQUIRE(pooled.clips.size() == serial.clips.size());
  REQUIRE(pooled.masks.vertices.size() == serial.masks.vertices.size());
  REQUIRE(pooled.masks.draws.size() == serial.masks.draws.size());

  usize mismatched = 0;
  for (usize i = 0; i < serial.geometry.vertices.size(); ++i)
    if (pooled.geometry.vertices[i].position !=
            serial.geometry.vertices[i].position ||
        pooled.geometry.vertices[i].uv != serial.geometry.vertices[i].uv)
      ++mismatched;
  for (usize i = 0; i < serial.geometry.indices.size(); ++i)
    if (pooled.geometry.indices[i] != serial.geometry.indices[i])
      ++mismatched;
  for (usize i = 0; i < serial.geometry.draws.size(); ++i) {
    const auto &a = serial.geometry.draws[i];
    const auto &b = pooled.geometry.draws[i];
    if (a.first_index != b.first_index || a.index_count != b.index_count ||
        a.vertex_offset != b.vertex_offset || a.texture != b.texture ||
        a.sort_key != b.sort_key)
      ++mismatched;
  }
  for (usize i = 0; i < serial.clips.size(); ++i) {
    const DrawMask &a = serial.clips[i];
    const DrawMask &b = pooled.clips[i];
    if (a.group != b.group || a.atlas != b.atlas || a.channel != b.channel ||
        a.inverted != b.inverted || a.from_world != b.from_world)
      ++mismatched;
  }
  for (usize i = 0; i < serial.masks.draws.size(); ++i) {
    const MaskDraw &a = serial.masks.draws[i];
    const MaskDraw &b = pooled.masks.draws[i];
    if (a.first_index != b.first_index || a.vertex_offset != b.vertex_offset ||
        a.atlas != b.atlas || a.channel != b.channel)
      ++mismatched;
  }
  CHECK(mismatched == 0u);
}

TEST_CASE("live2d: low memory shrinks live masks and future budget") {
  NX_REQUIRE_FIXTURE();
  World world;
  REQUIRE(world.ok);

  const scene::Entity e = world.place(MODEL);
  REQUIRE(world.system.load_pending(world.registry) == 1u);
  Live2DRuntime &runtime = world.registry.get<Live2DRuntime>(e);
  REQUIRE(runtime.masks.atlas_size() == 512u);
  const u64 before = world.system.mask_budget();

  CHECK(world.system.on_low_memory(world.registry) == 1u);
  CHECK(runtime.masks.atlas_size() == 256u);
  CHECK(world.system.mask_resolution_limit() == 256u);
  CHECK(world.system.mask_budget() < before);
  CHECK(runtime.masks.active());
}
