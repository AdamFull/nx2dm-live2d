
#include "framework/nxtest.h"

#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_assets.h"

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
