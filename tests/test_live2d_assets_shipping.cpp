#include "framework/nxtest.h"

#include "live2d/live2d_asset_bundle.h"
#include "live2d/live2d_assets.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/vfs/vfs.h"

namespace {

constexpr nx::string_view MODEL = "/live2d/Hiyori.model3.json";

[[nodiscard]] std::span<const u8> bytes(const nx::string_view text) {
  return {reinterpret_cast<const u8 *>(text.data()), text.size()};
}

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

struct MountedMemory {
  nx::vfs::MemoryDevice *files = nullptr;

  MountedMemory() {
    if (!nx::vfs::initialize())
      return;
    files = nx::vfs::make_memory_device();
    if (!nx::vfs::mount("/", files).valid())
      files = nullptr;
  }
  ~MountedMemory() { nx::vfs::shutdown(); }
};

[[nodiscard]] nx::blob<u8> blob_of(const nx::vector<u8> &source) {
  return nx::blob<u8>(source.begin(), source.end());
}

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

TEST_CASE("live2d assets: Shipping rejects Cubism-incompatible bundle JSON") {
  constexpr nx::string_view manifest =
      R"({"FileReferences":{"Moc":"hero.moc3"},"Version":3})";
  const nx::resource_bundle::Source resources[] = {
      {"hero.model3.json", bytes(manifest)}, {"hero.moc3", bytes("moc")}};
  const auto encoded =
      nxm::live2d::encode_model_bundle("hero.model3.json", resources);
  REQUIRE(encoded);

  MountedMemory mounted;
  REQUIRE(mounted.files != nullptr);
  mounted.files->add("/hero.model3.json.nxb", blob_of(encoded.value()));

  nxm::live2d::ModelAsset asset;
  nx::string error;
  CHECK_FALSE(nxm::live2d::load_model("/hero.model3.json", {}, asset, error));
  CHECK(error.find("Cubism JSON") != nx::string::npos);
  CHECK_FALSE(asset.valid());
}

TEST_CASE("live2d assets: Shipping rejects malformed embedded Cubism JSON") {
  constexpr nx::string_view manifest = R"({
    "Version": 3,
    "FileReferences": {
      "Moc": "hero.moc3",
      "Physics": "hero.physics3.json"
    }
  })";
  constexpr nx::string_view incompatible_physics =
      R"({"Meta":{"TotalInputCount":0}})";
  const nx::resource_bundle::Source resources[] = {
      {"hero.model3.json", bytes(manifest)},
      {"hero.moc3", bytes("moc")},
      {"hero.physics3.json", bytes(incompatible_physics)}};
  const auto encoded =
      nxm::live2d::encode_model_bundle("hero.model3.json", resources);
  REQUIRE(encoded);

  MountedMemory mounted;
  REQUIRE(mounted.files != nullptr);
  mounted.files->add("/hero.model3.json.nxb", blob_of(encoded.value()));

  nxm::live2d::ModelAsset asset;
  nx::string error;
  CHECK_FALSE(nxm::live2d::load_model("/hero.model3.json", {}, asset, error));
  CHECK(error.find("hero.physics3.json") != nx::string::npos);
  CHECK_FALSE(asset.valid());
}
