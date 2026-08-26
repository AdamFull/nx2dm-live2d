#include "live2d/live2d_assets.h"

#include "live2d/live2d_asset_bundle.h"
#include "live2d/live2d_platform.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/serialization/asset_policy.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/vfs.h"
#include "core/rendering/render2d/render_interop.h"

#include <CubismDefaultParameterId.hpp>
#include <CubismModelSettingJson.hpp>
#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Id/CubismIdManager.hpp>
#include <Live2DCubismCore.hpp>
#include <Model/CubismUserModel.hpp>
#include <Utils/CubismJson.hpp>

#include <limits>

namespace nxm::live2d {
namespace {

namespace csm = Live2D::Cubism::Framework;
namespace core = Live2D::Cubism::Core;

class HostedModel final : public csm::CubismUserModel {
public:
  using csm::CubismUserModel::_breath;
  using csm::CubismUserModel::_eyeBlink;
  using csm::CubismUserModel::_model;
  using csm::CubismUserModel::_physics;
  using csm::CubismUserModel::_pose;
};

[[nodiscard]] nx::string beside(const nx::string_view manifest,
                                const nx::string_view name) {
  const nx::string_view dir = nx::fs::path::parent_path(manifest);
  if (dir.empty() || dir == "/")
    return nx::format("/{}", name);
  return nx::format("{}/{}", dir, name);
}

[[nodiscard]] bool empty_name(const char *const name) noexcept {
  return name == nullptr || name[0] == '\0';
}

[[nodiscard]] bool ends_with(const nx::string_view value,
                             const nx::string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

struct LoadedResource {
  nx::blob<u8> storage;
  std::span<const u8> bytes;

  [[nodiscard]] explicit operator bool() const noexcept {
    return !bytes.empty();
  }
};

[[nodiscard]] bool cubism_json_valid(const std::span<const u8> bytes) {
  if (bytes.empty() ||
      bytes.size() >
          nx::cast<usize>(std::numeric_limits<csm::csmSizeInt>::max()))
    return false;
  csm::Utils::CubismJson *const json = csm::Utils::CubismJson::Create(
      bytes.data(), nx::cast<csm::csmSizeInt>(bytes.size()));
  if (json == nullptr)
    return false;
  csm::Utils::CubismJson::Delete(json);
  return true;
}

[[nodiscard]] LoadedResource read_resource(const ModelBundleView *const bundle,
                                           const nx::string_view model_path,
                                           const nx::string_view name) {
  if (!nx::resource_bundle::valid_name(name))
    return {};
  if (bundle != nullptr)
    return {.storage = {}, .bytes = bundle->resources.find(name)};
  const nx::string path = beside(model_path, name);
  auto bytes = nx::vfs::read(path.view());
  if (!bytes || bytes->empty() || bytes->size() > MAX_LIVE2D_RESOURCE_BYTES)
    return {};
  LoadedResource out;
  out.storage = std::move(bytes.value());
  out.bytes = {out.storage.data(), out.storage.size()};
  return out;
}

} // namespace

ModelAsset::~ModelAsset() { reset(); }

ModelAsset::ModelAsset(ModelAsset &&other) noexcept
    : m_owner(other.m_owner), m_textures(std::move(other.m_textures)),
      m_motions(std::move(other.m_motions)),
      m_expressions(std::move(other.m_expressions)),
      m_missing(std::move(other.m_missing)),
      m_lip_sync(std::move(other.m_lip_sync)),
      m_dependencies(std::move(other.m_dependencies)), m_canvas(other.m_canvas),
      m_physics(other.m_physics), m_pose(other.m_pose),
      m_eye_blink(other.m_eye_blink) {
  other.m_owner = nullptr;
}

ModelAsset &ModelAsset::operator=(ModelAsset &&other) noexcept {
  if (this != &other) {
    reset();
    m_owner = other.m_owner;
    m_textures = std::move(other.m_textures);
    m_motions = std::move(other.m_motions);
    m_expressions = std::move(other.m_expressions);
    m_missing = std::move(other.m_missing);
    m_lip_sync = std::move(other.m_lip_sync);
    m_dependencies = std::move(other.m_dependencies);
    m_canvas = other.m_canvas;
    m_physics = other.m_physics;
    m_pose = other.m_pose;
    m_eye_blink = other.m_eye_blink;
    other.m_owner = nullptr;
  }
  return *this;
}

void ModelAsset::reset() noexcept {
  for (const MotionEntry &entry : m_motions)
    csm::ACubismMotion::Delete(entry.motion);
  for (const ExpressionEntry &entry : m_expressions)
    csm::ACubismMotion::Delete(entry.motion);
  m_motions.clear();
  m_expressions.clear();

  if (m_owner != nullptr) {
    CSM_DELETE(static_cast<HostedModel *>(m_owner));
    m_owner = nullptr;
  }
  m_textures.clear();
  m_missing.clear();
  m_lip_sync.clear();
  m_dependencies.clear();
  m_canvas = {};
  m_physics = false;
  m_pose = false;
  m_eye_blink = false;
}

csm::CubismModel *ModelAsset::model() const noexcept {
  return m_owner == nullptr ? nullptr : m_owner->GetModel();
}

usize ModelAsset::parameter_count() const noexcept {
  const csm::CubismModel *const m = model();
  return m == nullptr
             ? 0u
             : nx::cast<usize>(
                   const_cast<csm::CubismModel *>(m)->GetParameterCount());
}

usize ModelAsset::part_count() const noexcept {
  const csm::CubismModel *const m = model();
  return m == nullptr ? 0u
                      : nx::cast<usize>(
                            const_cast<csm::CubismModel *>(m)->GetPartCount());
}

usize ModelAsset::drawable_count() const noexcept {
  const csm::CubismModel *const m = model();
  return m == nullptr
             ? 0u
             : nx::cast<usize>(
                   const_cast<csm::CubismModel *>(m)->GetDrawableCount());
}

csm::ACubismMotion *ModelAsset::find_motion(const nx::string_view group,
                                            const i32 index) const noexcept {
  for (const MotionEntry &entry : m_motions)
    if (entry.index == index && entry.group.view() == group)
      return entry.motion;
  return nullptr;
}

csm::ACubismMotion *
ModelAsset::find_expression(const nx::string_view name) const noexcept {
  for (const ExpressionEntry &entry : m_expressions)
    if (entry.name.view() == name)
      return entry.motion;
  return nullptr;
}

bool load_model(const nx::string_view model3_path, TextureResolver resolve,
                ModelAsset &out, nx::string &error) {
  if (!install_platform()) {
    error = "the Cubism framework would not start";
    return false;
  }
  out.reset();
  error.clear();

  nx::blob<u8> cooked_storage;
  nx::blob<u8> authored_storage;
  std::optional<ModelBundleView> bundle;
  std::span<const u8> manifest;
  const bool explicit_cooked = ends_with(model3_path, ".nxb");
  const nx::string authored_path =
      explicit_cooked
          ? nx::string(model3_path.substr(0, model3_path.size() - 4))
          : nx::string(model3_path);
  nx::string cooked_path(model3_path);
  if (!explicit_cooked)
    cooked_path += ".nxb";
  out.m_dependencies.push_back(cooked_path);
  const nx::vfs::FileInfo cooked_info = nx::vfs::stat(cooked_path.view());
  if (cooked_info.exists) {
    if (cooked_info.is_directory ||
        cooked_info.size > MAX_LIVE2D_BUNDLE_BYTES) {
      error =
          nx::format("cooked model '{}' exceeds its size limit", cooked_path);
      return false;
    }
    auto bytes = nx::vfs::read(cooked_path.view());
    if (bytes)
      cooked_storage = std::move(bytes.value());
    bundle = open_model_bundle({cooked_storage.data(), cooked_storage.size()});
    if (!bundle) {
      error = nx::format("cooked model '{}' is malformed", cooked_path);
      return false;
    }
    manifest = bundle->model_json();
  } else {
    if (explicit_cooked || !nx::asset_policy::can_fallback_to_authored_source(
                               cooked_info.exists)) {
      error = nx::format("no cooked model at '{}'", cooked_path);
      return false;
    }
    out.m_dependencies.push_back(authored_path);
    auto bytes = nx::vfs::read(authored_path.view());
    if (!bytes || bytes->empty() || bytes->size() > MAX_LIVE2D_MANIFEST_BYTES) {
      error = nx::format("no bounded model at '{}'", authored_path);
      return false;
    }
    authored_storage = std::move(bytes.value());
    manifest = {authored_storage.data(), authored_storage.size()};
    ModelManifest validated;
    if (!parse_model_manifest(
            nx::string_view(reinterpret_cast<const char *>(manifest.data()),
                            manifest.size()),
            validated, error)) {
      error = nx::format("{}: {}", authored_path, error);
      return false;
    }
  }

  // Several specialized Cubism JSON constructors do not remain safe after
  // their internal parser rejects a document. Validate the exact byte spans
  // first so a corrupt or parser-incompatible pack becomes a normal load
  // error instead of exposing an invalid SDK object.
  if (!cubism_json_valid(manifest)) {
    error = nx::format("{}: is not valid Cubism JSON", authored_path);
    return false;
  }
  if (bundle) {
    for (const nx::string &name : bundle->manifest.embedded) {
      if (!ends_with(name.view(), ".json"))
        continue;
      const std::span<const u8> bytes = bundle->resources.find(name.view());
      if (!cubism_json_valid(bytes)) {
        error =
            nx::format("{}: embedded resource '{}' is not valid Cubism JSON",
                       authored_path, name);
        return false;
      }
    }
  }

  csm::CubismModelSettingJson settings(
      const_cast<csm::csmByte *>(manifest.data()),
      nx::cast<csm::csmSizeInt>(manifest.size()));

  const char *const moc_name = settings.GetModelFileName();
  if (empty_name(moc_name)) {
    error = nx::format("{}: names no .moc3", authored_path);
    return false;
  }
  const nx::string moc_path = beside(authored_path.view(), moc_name);
  if (!bundle)
    out.m_dependencies.push_back(moc_path);
  const LoadedResource moc = read_resource(bundle ? &bundle.value() : nullptr,
                                           authored_path.view(), moc_name);
  if (!moc) {
    error = nx::format("no moc at '{}'", moc_path);
    return false;
  }

  const u32 version = nx::cast<u32>(core::csmGetMocVersion(
      moc.bytes.data(), nx::cast<unsigned int>(moc.bytes.size())));
  if (version == 0 || version > latest_moc_version()) {
    error = nx::format("{}: moc3 format {} and this Core reads up to {}",
                       moc_path, version, latest_moc_version());
    return false;
  }

  auto *const owner = CSM_NEW HostedModel();
  owner->LoadModel(moc.bytes.data(),
                   nx::cast<csm::csmSizeInt>(moc.bytes.size()), true);
  if (owner->GetModel() == nullptr) {
    CSM_DELETE(owner);
    error = nx::format("{}: Core refused the moc", moc_path);
    return false;
  }
  out.m_owner = owner;

  {
    core::csmVector2 size{};
    core::csmVector2 origin{};
    float units = 1.f;
    core::csmReadCanvasInfo(
        reinterpret_cast<const core::csmModel *>(owner->GetModel()->GetModel()),
        &size, &origin, &units);
    out.m_canvas = {size.X, size.Y, origin.X, origin.Y, units};
  }

  const auto read_optional = [&](const nx::string_view name,
                                 nx::string &path_out) {
    path_out = beside(authored_path.view(), name);
    if (!bundle)
      out.m_dependencies.push_back(path_out);
    LoadedResource bytes = read_resource(bundle ? &bundle.value() : nullptr,
                                         authored_path.view(), name);
    if (!bytes) {
      out.m_missing.push_back(path_out);
      nx::logw("live2d: {} names '{}', which is not there", authored_path,
               name);
    }
    return bytes;
  };

  const auto valid_optional_json = [&](const LoadedResource &resource,
                                       const nx::string_view resource_path) {
    if (bundle || cubism_json_valid(resource.bytes))
      return true;
    error = nx::format("{}: is not valid Cubism JSON", resource_path);
    out.reset();
    return false;
  };

  out.m_textures.reserve(nx::cast<usize>(settings.GetTextureCount()));
  for (i32 i = 0; i < settings.GetTextureCount(); ++i) {
    const char *const name = settings.GetTextureFileName(i);
    if (empty_name(name)) {
      out.m_textures.push_back(pack_texture(NX_TEXTURE_NONE, 0));
      continue;
    }
    const nx::string path = beside(authored_path.view(), name);
    out.m_dependencies.push_back(path);
    const u32 packed =
        resolve ? resolve(path) : pack_texture(NX_TEXTURE_NONE, 0);
    if ((packed >> 16) == NX_TEXTURE_NONE)
      nx::logw("live2d: no texture for '{}'; its drawables will be untextured",
               path);
    out.m_textures.push_back(packed);
  }

  nx::string path;
  if (const char *const name = settings.GetPhysicsFileName(); !empty_name(name))
    if (const auto bytes = read_optional(name, path)) {
      if (!valid_optional_json(bytes, path.view()))
        return false;
      owner->LoadPhysics(bytes.bytes.data(),
                         nx::cast<csm::csmSizeInt>(bytes.bytes.size()));
      out.m_physics = owner->_physics != nullptr;
    }

  if (const char *const name = settings.GetPoseFileName(); !empty_name(name))
    if (const auto bytes = read_optional(name, path)) {
      if (!valid_optional_json(bytes, path.view()))
        return false;
      owner->LoadPose(bytes.bytes.data(),
                      nx::cast<csm::csmSizeInt>(bytes.bytes.size()));
      out.m_pose = owner->_pose != nullptr;
    }

  if (const char *const name = settings.GetUserDataFile(); !empty_name(name))
    if (const auto bytes = read_optional(name, path)) {
      if (!valid_optional_json(bytes, path.view()))
        return false;
      owner->LoadUserData(bytes.bytes.data(),
                          nx::cast<csm::csmSizeInt>(bytes.bytes.size()));
    }

  for (i32 i = 0; i < settings.GetExpressionCount(); ++i) {
    const char *const file = settings.GetExpressionFileName(i);
    if (empty_name(file))
      continue;
    const auto bytes = read_optional(file, path);
    if (!bytes)
      continue;
    if (!valid_optional_json(bytes, path.view()))
      return false;
    const char *const name = settings.GetExpressionName(i);
    csm::ACubismMotion *const motion = owner->LoadExpression(
        bytes.bytes.data(), nx::cast<csm::csmSizeInt>(bytes.bytes.size()),
        name);
    if (motion != nullptr)
      out.m_expressions.push_back({nx::string(name), motion});
  }

  for (i32 g = 0; g < settings.GetMotionGroupCount(); ++g) {
    const char *const group = settings.GetMotionGroupName(g);
    for (i32 i = 0; i < settings.GetMotionCount(group); ++i) {
      const char *const file = settings.GetMotionFileName(group, i);
      if (empty_name(file))
        continue;
      const auto bytes = read_optional(file, path);
      if (!bytes)
        continue;
      if (!valid_optional_json(bytes, path.view()))
        return false;

      const nx::string name = nx::format("{}_{}", group, i);
      csm::ACubismMotion *const motion = owner->LoadMotion(
          bytes.bytes.data(), nx::cast<csm::csmSizeInt>(bytes.bytes.size()),
          name.c_str(), nullptr, nullptr, &settings, group, i);
      if (motion == nullptr)
        continue;
      const f32 fade_in = settings.GetMotionFadeInTimeValue(group, i);
      const f32 fade_out = settings.GetMotionFadeOutTimeValue(group, i);
      if (fade_in >= 0.f)
        motion->SetFadeInTime(fade_in);
      if (fade_out >= 0.f)
        motion->SetFadeOutTime(fade_out);
      out.m_motions.push_back({nx::string(group), i, motion});
    }
  }

  owner->_eyeBlink = csm::CubismEyeBlink::Create(&settings);
  out.m_eye_blink = owner->_eyeBlink != nullptr;

  for (i32 i = 0; i < settings.GetLipSyncParameterCount(); ++i)
    if (const csm::CubismIdHandle id = settings.GetLipSyncParameterId(i);
        id != nullptr)
      out.m_lip_sync.push_back(nx::string(id->GetString().GetRawString()));

  owner->_breath = csm::CubismBreath::Create();
  if (owner->_breath != nullptr) {
    csm::csmVector<csm::CubismBreath::BreathParameterData> breath;
    const auto id = [](const char *const name) {
      return csm::CubismFramework::GetIdManager()->GetId(name);
    };
    breath.PushBack({id("ParamAngleX"), 0.f, 15.f, 6.5345f, 0.5f});
    breath.PushBack({id("ParamAngleY"), 0.f, 8.f, 3.5345f, 0.5f});
    breath.PushBack({id("ParamAngleZ"), 0.f, 10.f, 5.5345f, 0.5f});
    breath.PushBack({id("ParamBodyAngleX"), 0.f, 4.f, 15.5345f, 0.5f});
    breath.PushBack({id("ParamBreath"), 0.5f, 0.5f, 3.2345f, 1.f});
    owner->_breath->SetParameters(breath);
  }

  owner->IsInitialized(true);
  nx::logi("live2d: '{}' - {} parameters, {} parts, {} drawables, {} motions, "
           "{} expressions{}{}",
           authored_path, out.parameter_count(), out.part_count(),
           out.drawable_count(), out.motions().size(), out.expressions().size(),
           out.has_physics() ? ", physics" : "",
           out.has_pose() ? ", pose" : "");
  if (!out.m_missing.empty())
    nx::logw("live2d: '{}' names {} file(s) that are not there", authored_path,
             out.m_missing.size());
  return true;
}

} // namespace nxm::live2d
