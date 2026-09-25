#include "framework/nxtest.h"

#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"
#include "core/foundation/threading/thread_pool.h"
#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_draw.h"
#include "live2d/live2d_platform.h"
#include "live2d/live2d_system.h"
#include "rendering/render2d/render_interop.h"

#include <CubismFramework.hpp>
#include <Id/CubismId.hpp>
#include <Id/CubismIdManager.hpp>

#include <atomic>
#include <chrono>

namespace {

using namespace nxm::live2d;
namespace scene = nxe::scene;
namespace csm = Live2D::Cubism::Framework;

constexpr nx::string_view MODEL = "/live2d/Hiyori.model3.json";
constexpr nx::string_view MISSING = "/live2d/Missing.model3.json";
constexpr u32 PAGE = 3;

[[nodiscard]] bool have_bundled() {
  return nx::fs::exists(nx::fs::path_view(
      nx::string(nx::string_view(NX_LIVE2D_BUNDLED_DIR)) + nx::string(MODEL)));
}

nx::thread_pool &io_threads() {
  static nx::thread_pool pool(
      {.thread_count = 2, .name = "nx-test-io", .blocking_threads = 5});
  return pool;
}

// A system that loads through a real VFS service and worker pool, over the
// bundled Hiyori.
struct Loader {
  bool ok = false;
  nx::thread_pool workers{{.thread_count = 2}};
  nx::unique_ptr<nx::vfs::AsyncIoService> io;
  scene::registry_t registry;
  Live2DSystem system;

  Loader() {
    nx::vfs::initialize();
    nx::vfs::Device *const host = nx::vfs::make_host_device(
        nx::fs::path_view(nx::string_view(NX_LIVE2D_BUNDLED_DIR)));
    ok = host != nullptr && nx::vfs::mount("/", host);
    Live2DSystem::register_components(registry);
    system.set_resolver(
        TextureResolver([](nx::string_view) { return pack_texture(PAGE, 0); }));
    io = nx::make_unique<nx::vfs::AsyncIoService>(
        io_threads(), nx::vfs::AsyncIoConfig{.max_concurrent_requests = 2});
    system.bind_loads(*io, workers);
  }
  ~Loader() {
    system.shutdown_loads();
    registry.clear();
    system.release_mocs();
    io.reset();
    nx::vfs::shutdown();
  }

  Loader(const Loader &) = delete;
  Loader &operator=(const Loader &) = delete;

  scene::Entity place(const nx::string_view model) {
    const scene::Entity e = registry.create();
    registry.emplace<Live2DModel>(e, Live2DModel{.model = nx::string(model)});
    return e;
  }

  // Frames until every load has been taken up, or a bound on them.
  usize settle() {
    usize loaded = 0;
    for (u32 frame = 0; frame < 2000; ++frame) {
      loaded += system.load_pending(registry, 1.f / 60.f);
      if (system.loads_in_flight() == 0)
        return loaded;
      nx::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return loaded;
  }
};

} // namespace

#define REQUIRE_BUNDLED()                                                      \
  do {                                                                         \
    if (!have_bundled())                                                       \
      SKIP("bundled Hiyori model absent from the build tree");                 \
  } while (false)

TEST_CASE("live2d loading: a bound system loads off the frame, then takes it") {
  REQUIRE_BUNDLED();
  Loader loader;
  REQUIRE(loader.ok);

  const scene::Entity e = loader.place(MODEL);
  // Nothing loads on the calling thread: the model is only asked for.
  CHECK(loader.system.load_pending(loader.registry) == 0u);
  CHECK(loader.system.loads_in_flight() == 1u);
  const Live2DRuntime *runtime = loader.registry.try_get<Live2DRuntime>(e);
  REQUIRE(runtime != nullptr);
  CHECK_FALSE(runtime->ready());
  CHECK(runtime->requested == MODEL);

  CHECK(loader.settle() == 1u);
  runtime = loader.registry.try_get<Live2DRuntime>(e);
  REQUIRE(runtime != nullptr);
  CHECK(runtime->ready());
  CHECK(runtime->loaded == MODEL);
  CHECK(runtime->masks.active() == (masked_drawable_count(runtime->asset) > 0));

  // What it built is what a load on this thread builds.
  ModelAsset direct;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, direct, error));
  CHECK(runtime->asset.drawable_count() == direct.drawable_count());
  CHECK(runtime->asset.parameter_count() == direct.parameter_count());
  CHECK(runtime->asset.motions().size() == direct.motions().size());
  REQUIRE(runtime->asset.textures().size() == direct.textures().size());
  for (const u32 packed : runtime->asset.textures())
    CHECK((packed >> 16) == PAGE);
  CHECK(runtime->asset.dependencies().size() == direct.dependencies().size());
  CHECK(runtime->asset.mesh()->vertex_count() == direct.mesh()->vertex_count());
}

TEST_CASE("live2d loading: models of one file loading at once share one moc") {
  REQUIRE_BUNDLED();
  Loader loader;
  REQUIRE(loader.ok);

  constexpr u32 COUNT = 12;
  nx::vector<scene::Entity> placed;
  for (u32 i = 0; i < COUNT; ++i)
    placed.push_back(loader.place(MODEL));
  REQUIRE(loader.settle() == COUNT);

  const SharedMoc *const shared =
      loader.registry.get<Live2DRuntime>(placed[0]).asset.moc().get();
  REQUIRE(shared != nullptr);
  usize others = 0;
  for (const scene::Entity e : placed)
    others += loader.registry.get<Live2DRuntime>(e).asset.moc().get() != shared
                  ? 1u
                  : 0u;
  CHECK(others == 0u);
}

TEST_CASE("live2d loading: a model changed mid-load drops what was loading") {
  REQUIRE_BUNDLED();
  Loader loader;
  REQUIRE(loader.ok);

  const scene::Entity e = loader.place(MODEL);
  (void)loader.system.load_pending(loader.registry);
  REQUIRE(loader.system.loads_in_flight() == 1u);

  // The load of the model it no longer wants is never published.
  loader.registry.get<Live2DModel>(e).model = nx::string(MISSING);
  CHECK(loader.settle() == 0u);
  const Live2DRuntime &runtime = loader.registry.get<Live2DRuntime>(e);
  CHECK(runtime.requested == MISSING);
  CHECK_FALSE(runtime.ready());
  CHECK(runtime.loaded.empty());
  CHECK(runtime.load_failures == 1u);
  CHECK(runtime.retry_in > 0.f);

  // And back: the first model loads afresh.
  loader.registry.get<Live2DModel>(e).model = nx::string(MODEL);
  CHECK(loader.settle() == 1u);
  CHECK(loader.registry.get<Live2DRuntime>(e).loaded == MODEL);
}

TEST_CASE("live2d loading: a model gone mid-load leaves nothing behind") {
  REQUIRE_BUNDLED();
  Loader loader;
  REQUIRE(loader.ok);

  const scene::Entity gone = loader.place(MODEL);
  const scene::Entity kept = loader.place(MODEL);
  (void)loader.system.load_pending(loader.registry);
  REQUIRE(loader.system.loads_in_flight() == 2u);
  loader.registry.destroy(gone);

  CHECK(loader.settle() == 1u);
  CHECK(loader.system.loads_in_flight() == 0u);
  CHECK(loader.registry.get<Live2DRuntime>(kept).ready());
}

TEST_CASE("live2d loading: shutting down with loads in flight drains them") {
  REQUIRE_BUNDLED();
  Loader loader;
  REQUIRE(loader.ok);

  for (u32 i = 0; i < 4; ++i)
    (void)loader.place(MODEL);
  (void)loader.system.load_pending(loader.registry);
  CHECK(loader.system.loads_in_flight() == 4u);
  loader.system.shutdown_loads();
  CHECK(loader.system.loads_in_flight() == 0u);
}

TEST_CASE("live2d loading: a page that arrives after the load is taken up, not "
          "reloaded") {
  REQUIRE_BUNDLED();
  u32 answer = pack_texture(NX_TEXTURE_NONE, 0);
  Loader loader;
  REQUIRE(loader.ok);
  loader.system.set_resolver(
      TextureResolver([&answer](nx::string_view) { return answer; }));

  const scene::Entity e = loader.place(MODEL);
  REQUIRE(loader.settle() == 1u);
  const Live2DRuntime &runtime = loader.registry.get<Live2DRuntime>(e);
  REQUIRE_FALSE(runtime.asset.textures().empty());
  for (const u32 packed : runtime.asset.textures())
    CHECK((packed >> 16) == NX_TEXTURE_NONE);
  const auto *const model = runtime.asset.model();
  CHECK(loader.system.refresh_textures(loader.registry) == 0u);

  answer = pack_texture(PAGE, 0);
  CHECK(loader.system.refresh_textures(loader.registry) == 1u);
  CHECK(loader.system.refresh_textures(loader.registry) == 0u);
  for (const u32 packed : runtime.asset.textures())
    CHECK((packed >> 16) == PAGE);
  CHECK(runtime.asset.model() == model);
}

TEST_CASE("live2d loading: ids registered from many threads stay one table") {
  REQUIRE(install_platform());
  constexpr u32 THREADS = 8;
  constexpr u32 NAMES = 400;
  // Every thread registers the same names, interleaved with others' first
  // registrations, so a table that grows under a reader shows.
  nx::vector<nx::vector<const csm::CubismId *>> seen(THREADS);
  std::atomic<u32> ready{0};
  {
    nx::vector<nx::unique_ptr<nx::thread>> threads;
    for (u32 t = 0; t < THREADS; ++t)
      threads.push_back(nx::make_unique<nx::thread>([&, t] {
        ready.fetch_add(1);
        while (ready.load() < THREADS)
          nx::this_thread::yield();
        seen[t].resize(NAMES);
        for (u32 n = 0; n < NAMES; ++n) {
          const u32 name = (n + t * 37u) % NAMES;
          const nx::string id = nx::format("ParamLoadingTest{}", name);
          seen[t][name] =
              csm::CubismFramework::GetIdManager()->GetId(id.c_str());
        }
      }));
  }

  usize disagreements = 0;
  for (u32 n = 0; n < NAMES; ++n)
    for (u32 t = 1; t < THREADS; ++t)
      disagreements += seen[t][n] != seen[0][n] ? 1u : 0u;
  CHECK(disagreements == 0u);
  usize wrong = 0;
  for (u32 n = 0; n < NAMES; ++n)
    wrong +=
        seen[0][n]->GetString() != nx::format("ParamLoadingTest{}", n).c_str()
            ? 1u
            : 0u;
  CHECK(wrong == 0u);
}
