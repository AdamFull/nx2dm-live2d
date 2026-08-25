#include "assetc/cooker_registry.h"

#include "live2d/live2d_asset_bundle.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/serialization/json_document.h"
#include "core/foundation/strings/format.h"

#include <cstdio>
#include <cstring>

namespace assetc {
namespace {

inline constexpr nx::asset_contract::Format LIVE2D_FORMATS[] = {
    {".model3.json", ".model3.json.nxb", nxm::live2d::LIVE2D_BUNDLE_FORMAT,
     true},
};
inline constexpr nx::string_view LIVE2D_EMBEDDED[] = {
    ".moc3",           ".motion3.json", ".physics3.json", ".pose3.json",
    ".userdata3.json", ".cdi3.json",    ".exp3.json",
};

struct OwnedResource {
  nx::string name;
  nx::blob<u8> bytes;
};

struct Inputs {
  nx::string manifest_name;
  nxm::live2d::ModelManifest manifest;
  nx::vector<OwnedResource> resources;
};

[[nodiscard]] bool ends_with(const nx::string_view value,
                             const nx::string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool normalize_json_resource(OwnedResource &resource) {
  if (!ends_with(resource.name.view(), ".json"))
    return true;
  const nx::string_view text(
      reinterpret_cast<const char *>(resource.bytes.data()),
      resource.bytes.size());
  auto normalized = nx::json::normalize_asset_document(text);
  if (!normalized)
    return false;
  nx::blob<u8> bytes(normalized->size());
  std::memcpy(bytes.data(), normalized->data(), normalized->size());
  resource.bytes = std::move(bytes);
  return true;
}

[[nodiscard]] bool read_inputs(const nx::string_view source, Inputs &out,
                               nx::string &error) {
  auto model = nx::fs::file_read(source);
  if (!model || model->empty() ||
      model->size() > nxm::live2d::MAX_LIVE2D_MANIFEST_BYTES) {
    error = "model3 manifest is missing or exceeds its size limit";
    return false;
  }
  Inputs loaded;
  const nx::string_view authored_model_text(
      reinterpret_cast<const char *>(model->data()), model->size());
  auto normalized_model =
      nx::json::normalize_asset_document(authored_model_text);
  if (!normalized_model ||
      !nxm::live2d::parse_model_manifest(normalized_model->view(),
                                         loaded.manifest, error))
    return false;
  loaded.manifest_name = nx::string(nx::fs::path::filename(source));
  loaded.resources.reserve(loaded.manifest.embedded.size() + 1u);
  nx::blob<u8> manifest_bytes(normalized_model->size());
  std::memcpy(manifest_bytes.data(), normalized_model->data(),
              normalized_model->size());
  loaded.resources.push_back(
      {loaded.manifest_name, std::move(manifest_bytes)});

  const nx::string_view parent = nx::fs::path::parent_path(source);
  usize total = loaded.resources.front().bytes.size();
  for (const nx::string &name : loaded.manifest.embedded) {
    const nx::string path = nx::fs::path_view(parent) / name.view();
    auto bytes = nx::fs::file_read(path.view());
    if (!bytes || bytes->empty() ||
        bytes->size() > nxm::live2d::MAX_LIVE2D_RESOURCE_BYTES ||
        total > nxm::live2d::MAX_LIVE2D_BUNDLE_BYTES - bytes->size()) {
      error = nx::format("cannot read bounded model resource '{}'", path);
      return false;
    }
    loaded.resources.push_back({name, std::move(bytes.value())});
    if (!normalize_json_resource(loaded.resources.back())) {
      error = nx::format("model resource '{}' is not valid JSON", path);
      return false;
    }
    total += loaded.resources.back().bytes.size();
  }
  for (const nx::string &name : loaded.manifest.textures) {
    const nx::string path = nx::fs::path_view(parent) / name.view();
    const nx::fs::file_stat texture = nx::fs::stat_file(path.view());
    if (texture.type != nx::fs::file_type::regular || texture.size == 0 ||
        texture.size > nxm::live2d::MAX_LIVE2D_RESOURCE_BYTES) {
      error = nx::format("cannot find bounded model texture '{}'", path);
      return false;
    }
  }
  out = std::move(loaded);
  return true;
}

[[nodiscard]] nx::vector<nx::resource_bundle::Source>
source_views(const Inputs &inputs) {
  nx::vector<nx::resource_bundle::Source> sources;
  sources.reserve(inputs.resources.size());
  for (const OwnedResource &resource : inputs.resources)
    sources.push_back(
        {resource.name.view(), {resource.bytes.data(), resource.bytes.size()}});
  return sources;
}

[[nodiscard]] bool cook_live2d(const CookContext &context) {
  Inputs inputs;
  nx::string error;
  if (!read_inputs(context.source, inputs, error)) {
    std::fprintf(stderr, "assetc: Live2D '%.*s' is invalid: %.*s\n",
                 static_cast<int>(context.source.size()), context.source.data(),
                 static_cast<int>(error.size()), error.data());
    return false;
  }
  const auto sources = source_views(inputs);
  const auto cooked =
      nxm::live2d::encode_model_bundle(inputs.manifest_name.view(), sources);
  return cooked && static_cast<bool>(nx::fs::file_write_atomic(
                       context.output, {cooked->data(), cooked->size()}));
}

void hash_live2d_dependencies(nx::fnv1a64 &hash, const CookContext &context) {
  Inputs inputs;
  nx::string error;
  if (!read_inputs(context.source, inputs, error)) {
    hash.combine_bytes(error.data(), error.size());
    return;
  }
  for (usize i = 1; i < inputs.resources.size(); ++i) {
    const OwnedResource &resource = inputs.resources[i];
    hash.combine_bytes(resource.name.data(), resource.name.size());
    hash.combine_bytes(reinterpret_cast<const char *>(resource.bytes.data()),
                       resource.bytes.size());
  }
}

} // namespace

bool nx_assetc_register_live2d(CookerRegistry &registry, nx::string &error) {
  return registry.add({.name = "live2d",
                       .formats = LIVE2D_FORMATS,
                       .version = nxm::live2d::LIVE2D_BUNDLE_VERSION,
                       .cook = cook_live2d,
                       .contribute_hash = hash_live2d_dependencies,
                       .embedded_sources = LIVE2D_EMBEDDED},
                      error);
}

} // namespace assetc
