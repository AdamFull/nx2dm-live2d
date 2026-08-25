#pragma once

#include "core/foundation/serialization/nva.h"
#include "core/foundation/serialization/resource_bundle.h"
#include "core/foundation/strings/utf8_string.h"

#include <optional>
#include <span>

namespace nxm::live2d {

inline constexpr u32 LIVE2D_BUNDLE_FORMAT = nx::nva::fourcc('N', 'X', 'L', '2');
inline constexpr u16 LIVE2D_BUNDLE_VERSION = 1;
inline constexpr usize MAX_LIVE2D_MANIFEST_BYTES = 1024u * 1024u;
inline constexpr usize MAX_LIVE2D_RESOURCE_BYTES = 128u * 1024u * 1024u;
inline constexpr usize MAX_LIVE2D_BUNDLE_BYTES = 256u * 1024u * 1024u;
inline constexpr u32 MAX_LIVE2D_RESOURCES = 4096;

struct ModelManifest {
  nx::vector<nx::string> embedded;
  nx::vector<nx::string> textures;
};

/// Validates the Cubism 3 file-reference surface used by the runtime and
/// returns every resource that belongs in the atomic model bundle. Texture
/// names are validated but remain in the engine's texture pipeline.
[[nodiscard]] bool parse_model_manifest(nx::string_view text,
                                        ModelManifest &out, nx::string &error);

[[nodiscard]] std::optional<nx::vector<u8>>
encode_model_bundle(nx::string_view manifest_name,
                    std::span<const nx::resource_bundle::Source> resources);

struct ModelBundleView {
  nx::string manifest_name;
  ModelManifest manifest;
  nx::resource_bundle::View resources;

  [[nodiscard]] std::span<const u8> model_json() const noexcept {
    return resources.find(manifest_name.view());
  }
};

[[nodiscard]] std::optional<ModelBundleView>
open_model_bundle(std::span<const u8> bytes);

} // namespace nxm::live2d
