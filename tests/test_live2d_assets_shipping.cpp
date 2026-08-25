#include "framework/nxtest.h"

#include "live2d/live2d_assets.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/vfs/vfs.h"

namespace {

constexpr nx::string_view MODEL = "/live2d/Hiyori.model3.json";

struct MountedHost {
  bool ok = false;

  explicit MountedHost(const nx::string_view root) {
    if (!nx::vfs::initialize())
      return;
    nx::vfs::Device *const host =
        nx::vfs::make_host_device(nx::fs::path_view(root));
    ok = host != nullptr && nx::vfs::mount("/", host).valid();
  }
  ~MountedHost() { nx::vfs::shutdown(); }
};

} // namespace

TEST_CASE("live2d assets: Shipping loads the atomic cooked model") {
  MountedHost files(NX_LIVE2D_COOKED_DIR);
  REQUIRE(files.ok);
  nxm::live2d::ModelAsset asset;
  nx::string error;
  REQUIRE(nxm::live2d::load_model(MODEL, {}, asset, error));
  CHECK(error.empty());
  CHECK(asset.valid());
  CHECK(asset.drawable_count() > 0u);
  CHECK(asset.has_physics());
  CHECK(!asset.motions().empty());
}

TEST_CASE("live2d assets: Shipping rejects an authored-only model tree") {
  MountedHost files(NX_LIVE2D_AUTHORED_DIR);
  REQUIRE(files.ok);
  nxm::live2d::ModelAsset asset;
  nx::string error;
  CHECK_FALSE(nxm::live2d::load_model(MODEL, {}, asset, error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(asset.valid());
}
