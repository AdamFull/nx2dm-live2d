#include "framework/nxtest.h"

#include "live2d/live2d_asset_bundle.h"

namespace {

[[nodiscard]] std::span<const u8> bytes(const nx::string_view text) {
  return {reinterpret_cast<const u8 *>(text.data()), text.size()};
}

constexpr nx::string_view MANIFEST = R"({
  "Version": 3,
  "FileReferences": {
    "Moc": "hero.moc3",
    "Textures": ["textures/hero.png"],
    "Physics": "hero.physics3.json",
    "Expressions": [{"Name":"smile","File":"smile.exp3.json"}],
    "Motions": {"Idle":[{"File":"motions/idle.motion3.json"}]}
  }
})";

} // namespace

TEST_CASE("live2d bundle: manifest references become an atomic container") {
  nxm::live2d::ModelManifest manifest;
  nx::string error;
  REQUIRE(nxm::live2d::parse_model_manifest(MANIFEST, manifest, error));
  CHECK(manifest.embedded.size() == 4u);
  REQUIRE(manifest.textures.size() == 1u);
  CHECK(manifest.textures[0] == "textures/hero.png");

  const nx::resource_bundle::Source resources[] = {
      {"hero.model3.json", bytes(MANIFEST)},
      {"hero.moc3", bytes("moc")},
      {"hero.physics3.json", bytes("{}")},
      {"smile.exp3.json", bytes("{}")},
      {"motions/idle.motion3.json", bytes("{}")},
  };
  const auto encoded =
      nxm::live2d::encode_model_bundle("hero.model3.json", resources);
  REQUIRE(encoded);
  const auto opened =
      nxm::live2d::open_model_bundle({encoded->data(), encoded->size()});
  REQUIRE(opened);
  CHECK(opened->manifest_name == "hero.model3.json");
  CHECK(opened->resources.size() == 5u);
  CHECK(opened->resources.find("hero.moc3").size() == 3u);
}

TEST_CASE("live2d bundle: unsafe references and incomplete bundles fail") {
  constexpr nx::string_view unsafe = R"({
    "Version":3,
    "FileReferences":{"Moc":"../escape.moc3"}
  })";
  nxm::live2d::ModelManifest manifest;
  nx::string error;
  CHECK_FALSE(nxm::live2d::parse_model_manifest(unsafe, manifest, error));
  CHECK_FALSE(error.empty());

  const nx::resource_bundle::Source wrong_manifest[] = {
      {"hero.json", bytes(MANIFEST)}};
  CHECK_FALSE(nxm::live2d::encode_model_bundle("hero.json", wrong_manifest));

  const nx::resource_bundle::Source incomplete[] = {
      {"hero.model3.json", bytes(MANIFEST)}, {"hero.moc3", bytes("moc")}};
  const auto encoded =
      nxm::live2d::encode_model_bundle("hero.model3.json", incomplete);
  REQUIRE(encoded);
  CHECK_FALSE(nxm::live2d::open_model_bundle({encoded->data(), encoded->size()})
                  .has_value());
}
