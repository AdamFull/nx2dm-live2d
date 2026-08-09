/**
 * @file test_live2d_assets.cpp
 * @brief Reading a .model3.json and everything it names.
 */

#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/vfs/vfs.h"
#include "core/rendering/render2d/render_interop.h"
#include "live2d/live2d_assets.h"

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;

/// Mounts the fixture directory at the VFS root for one case, and takes it back
/// down however the case leaves.
struct Mounted {
  bool ok = false;

  Mounted() {
    nx::vfs::initialize();
    nx::vfs::Device *const host =
        nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
    ok = host != nullptr && nx::vfs::mount("/", host);
  }
  ~Mounted() { nx::vfs::shutdown(); }

  Mounted(const Mounted &) = delete;
  Mounted &operator=(const Mounted &) = delete;
};

/// Hands back a distinct index per path, so a case can tell the pages apart
/// and see that the order the manifest listed them in survived.
[[nodiscard]] TextureResolver counting(nx::vector<nx::string> &seen) {
  return TextureResolver([&seen](const nx::string_view path) {
    seen.push_back(nx::string(path));
    return pack_texture(nx::cast<u32>(seen.size()), 0);
  });
}

} // namespace

TEST_CASE("live2d: a model3.json brings its moc, textures and motions") {
  NX_REQUIRE_FIXTURE();
  Mounted mount;
  REQUIRE(mount.ok);

  nx::vector<nx::string> pages;
  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, counting(pages), asset, error));
  CHECK(error.empty());
  REQUIRE(asset.valid());

  // A model at all: a moc that produced parameters, parts and drawables.
  CHECK(asset.parameter_count() > 0u);
  CHECK(asset.part_count() > 0u);
  CHECK(asset.drawable_count() > 0u);

  // Every texture the manifest listed was offered to the resolver, in order,
  // and the answer was kept - a drawable's texture index means an index into
  // exactly this.
  REQUIRE(asset.textures().size() == pages.size());
  REQUIRE(!pages.empty());
  for (usize i = 0; i < pages.size(); ++i) {
    CHECK(pages[i].view().starts_with("/Frieren/"));
    CHECK((asset.textures()[i] >> 16) == nx::cast<u32>(i + 1));
  }

  CHECK(asset.has_physics());
  CHECK(!asset.motions().empty());
  CHECK(!asset.expressions().empty());
}

TEST_CASE("live2d: the canvas the moc declares is a real size") {
  NX_REQUIRE_FIXTURE();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));

  // Absolute, not a comparison against another load. This is what L2's
  // deformation is checked to stay inside, so a zero or a mirrored canvas here
  // would make that check vacuous rather than failing it.
  const CanvasInfo canvas = asset.canvas();
  CHECK(canvas.width > 0.f);
  CHECK(canvas.height > 0.f);
  CHECK(canvas.pixels_per_unit > 0.f);
  CHECK(canvas.origin_x > 0.f);
  CHECK(canvas.origin_y > 0.f);
}

TEST_CASE("live2d: a manifest naming a file that is not there still loads") {
  NX_REQUIRE_FIXTURE();
  Mounted mount;
  REQUIRE(mount.ok);

  // Built rather than borrowed. The development model happens to list two
  // expressions it does not ship, but a case that relies on that passes
  // vacuously against a clean export - so the broken manifest is made here and
  // overlaid on the real moc, which falls through to the host mount beneath.
  nx::vfs::MemoryDevice *const overlay = nx::vfs::make_memory_device();
  REQUIRE(overlay != nullptr);
  const nx::string_view manifest = R"({
    "Version": 3,
    "FileReferences": {
      "Moc": "Frieren.moc3",
      "Textures": ["Frieren.8192/texture_00.png"],
      "Physics": "no-such-physics3.json",
      "Expressions": [{ "Name": "gone", "File": "no-such.exp3.json" }],
      "Motions": { "": [{ "File": "no-such.motion3.json" }] }
    }
  })";
  const auto *const first = reinterpret_cast<const u8 *>(manifest.data());
  overlay->add("/Frieren/broken.model3.json",
               nx::blob<u8>(first, first + manifest.size()));
  REQUIRE(nx::vfs::mount("/", overlay, 100).valid());

  ModelAsset broken;
  nx::string error;
  REQUIRE(load_model("/Frieren/broken.model3.json", {}, broken, error));
  CHECK(broken.valid());
  CHECK(broken.drawable_count() > 0u);

  // Every one of the three was named, and none of them stopped the moc.
  CHECK(broken.missing().size() == 3u);
  CHECK(broken.expressions().empty());
  CHECK(broken.motions().empty());
  CHECK_FALSE(broken.has_physics());
  for (const nx::string &path : broken.missing())
    CHECK(path.view().starts_with("/Frieren/no-such"));
}

TEST_CASE("live2d: what loaded is reachable by the name the manifest gave it") {
  NX_REQUIRE_FIXTURE();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));

  for (const ExpressionEntry &entry : asset.expressions()) {
    REQUIRE(entry.motion != nullptr);
    CHECK(asset.find_expression(entry.name.view()) == entry.motion);
  }
  CHECK(asset.find_expression("nothing-by-this-name") == nullptr);

  for (const MotionEntry &entry : asset.motions()) {
    REQUIRE(entry.motion != nullptr);
    CHECK(asset.find_motion(entry.group.view(), entry.index) == entry.motion);
  }
  CHECK(asset.find_motion("nothing-by-this-name", 0) == nullptr);
}

TEST_CASE("live2d: a model that is not there is refused, and says so") {
  NX_REQUIRE_FIXTURE();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  CHECK_FALSE(load_model("/nothing/here.model3.json", {}, asset, error));
  CHECK(!error.empty());
  CHECK_FALSE(asset.valid());
}

TEST_CASE("live2d: loading twice into one asset releases the first") {
  NX_REQUIRE_FIXTURE();
  Mounted mount;
  REQUIRE(mount.ok);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));
  const usize drawables = asset.drawable_count();
  const usize motions = asset.motions().size();

  // The motions and expressions are the asset's to delete, and a second load
  // over the top is where they would leak or be deleted twice.
  REQUIRE(load_model(MODEL, {}, asset, error));
  CHECK(asset.drawable_count() == drawables);
  CHECK(asset.motions().size() == motions);
}
