#include "live2d/live2d_asset_bundle.h"

#include "core/foundation/serialization/json.h"

#include <algorithm>

namespace nxm::live2d {
namespace {

[[nodiscard]] bool ends_with(const nx::string_view value,
                             const nx::string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool fail(nx::string &error, const nx::string_view message) {
  error = nx::string(message);
  return false;
}

[[nodiscard]] bool append_reference(const nx::json::value *const value,
                                    const nx::string_view suffix,
                                    nx::vector<nx::string> &out,
                                    nx::string &error) {
  if (value == nullptr || !value->is_string())
    return fail(error, "a Cubism file reference is not a string");
  const nx::string_view name = value->as_string().view();
  if (!nx::resource_bundle::valid_name(name) || !ends_with(name, suffix))
    return fail(error, "a Cubism file reference has an unsafe name or suffix");
  out.push_back(nx::string(name));
  return true;
}

[[nodiscard]] nx::resource_bundle::Limits limits() noexcept {
  return {.max_entries = MAX_LIVE2D_RESOURCES,
          .max_name_bytes = 4096,
          .max_metadata_bytes = 4096,
          .max_data_bytes = MAX_LIVE2D_BUNDLE_BYTES};
}

} // namespace

bool parse_model_manifest(const nx::string_view text, ModelManifest &out,
                          nx::string &error) {
  error.clear();
  try {
    const nx::json::parse_options options{.reject_duplicate_keys = true,
                                          .validate_utf8 = true};
    auto parsed = nx::json::parse(text, options);
    if (!parsed || !parsed.value().is_object())
      return fail(error, "the model3 document is not a valid JSON object");
    const nx::json::value &root = parsed.value();
    const nx::json::value *const version = root.find("Version");
    const bool version_three =
        version != nullptr && ((version->is_u64() && version->as_u64() == 3) ||
                               (version->is_i64() && version->as_i64() == 3));
    if (!version_three)
      return fail(error, "the model3 Version must be 3");
    const nx::json::value *const refs = root.find("FileReferences");
    if (refs == nullptr || !refs->is_object())
      return fail(error, "the model3 document has no FileReferences object");

    ModelManifest decoded;
    if (!append_reference(refs->find("Moc"), ".moc3", decoded.embedded, error))
      return false;

    if (const nx::json::value *const textures = refs->find("Textures")) {
      if (!textures->is_array())
        return fail(error, "model3 Textures must be an array");
      for (const nx::json::value &texture : textures->as_array())
        if (!append_reference(&texture, ".png", decoded.textures, error))
          return false;
    }

    struct OptionalFile {
      nx::string_view field;
      nx::string_view suffix;
    };
    static constexpr OptionalFile optional[] = {{"Physics", ".physics3.json"},
                                                {"Pose", ".pose3.json"},
                                                {"UserData", ".userdata3.json"},
                                                {"DisplayInfo", ".cdi3.json"}};
    for (const OptionalFile &file : optional)
      if (const nx::json::value *const value = refs->find(file.field))
        if (!append_reference(value, file.suffix, decoded.embedded, error))
          return false;

    if (const nx::json::value *const expressions = refs->find("Expressions")) {
      if (!expressions->is_array())
        return fail(error, "model3 Expressions must be an array");
      for (const nx::json::value &entry : expressions->as_array()) {
        if (!entry.is_object() ||
            !append_reference(entry.find("File"), ".exp3.json",
                              decoded.embedded, error))
          return false;
      }
    }

    if (const nx::json::value *const motions = refs->find("Motions")) {
      if (!motions->is_object())
        return fail(error, "model3 Motions must be an object");
      for (const nx::json::member &group : motions->as_object()) {
        if (!group.val.is_array())
          return fail(error, "a model3 motion group is not an array");
        for (const nx::json::value &entry : group.val.as_array()) {
          if (!entry.is_object() ||
              !append_reference(entry.find("File"), ".motion3.json",
                                decoded.embedded, error))
            return false;
        }
      }
    }

    std::sort(decoded.embedded.begin(), decoded.embedded.end());
    decoded.embedded.erase(
        std::unique(decoded.embedded.begin(), decoded.embedded.end()),
        decoded.embedded.end());
    std::sort(decoded.textures.begin(), decoded.textures.end());
    decoded.textures.erase(
        std::unique(decoded.textures.begin(), decoded.textures.end()),
        decoded.textures.end());
    if (decoded.embedded.size() + 1u > MAX_LIVE2D_RESOURCES)
      return fail(error, "the model3 document references too many resources");
    out = std::move(decoded);
    return true;
  } catch (...) {
    return fail(error, "resource exhaustion while parsing model3 document");
  }
}

std::optional<nx::vector<u8>> encode_model_bundle(
    const nx::string_view manifest_name,
    const std::span<const nx::resource_bundle::Source> resources) {
  if (!nx::resource_bundle::valid_name(manifest_name) ||
      !ends_with(manifest_name, ".model3.json"))
    return std::nullopt;
  nx::nva::Writer metadata;
  metadata.str(manifest_name);
  return nx::resource_bundle::encode(LIVE2D_BUNDLE_FORMAT,
                                     LIVE2D_BUNDLE_VERSION, metadata.span(),
                                     resources, limits());
}

std::optional<ModelBundleView>
open_model_bundle(const std::span<const u8> bytes) {
  auto resources = nx::resource_bundle::View::open(
      bytes, LIVE2D_BUNDLE_FORMAT, LIVE2D_BUNDLE_VERSION, limits());
  if (!resources)
    return std::nullopt;
  nx::nva::Reader metadata(resources->metadata());
  nx::string manifest_name(metadata.str());
  if (!metadata.ok() || metadata.remaining() != 0 ||
      !nx::resource_bundle::valid_name(manifest_name.view()) ||
      !ends_with(manifest_name.view(), ".model3.json"))
    return std::nullopt;
  const std::span<const u8> model = resources->find(manifest_name.view());
  if (model.empty() || model.size() > MAX_LIVE2D_MANIFEST_BYTES)
    return std::nullopt;
  ModelManifest manifest;
  nx::string error;
  if (!parse_model_manifest(
          nx::string_view(reinterpret_cast<const char *>(model.data()),
                          model.size()),
          manifest, error) ||
      resources->size() != manifest.embedded.size() + 1u)
    return std::nullopt;
  for (const nx::string &name : manifest.embedded)
    if (resources->find(name.view()).empty())
      return std::nullopt;
  return ModelBundleView{std::move(manifest_name), std::move(manifest),
                         std::move(resources.value())};
}

} // namespace nxm::live2d
