#include "live2d/live2d_assets.h"

#include "live2d/live2d_platform.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
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

} // namespace

ModelAsset::~ModelAsset() { reset(); }

ModelAsset::ModelAsset(ModelAsset &&other) noexcept
    : m_owner(other.m_owner), m_textures(std::move(other.m_textures)),
      m_motions(std::move(other.m_motions)),
      m_expressions(std::move(other.m_expressions)),
      m_missing(std::move(other.m_missing)), m_canvas(other.m_canvas),
      m_physics(other.m_physics), m_pose(other.m_pose) {
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
    m_canvas = other.m_canvas;
    m_physics = other.m_physics;
    m_pose = other.m_pose;
    other.m_owner = nullptr;
  }
  return *this;
}

void ModelAsset::reset() noexcept {
  // The motions and expressions are ours: CubismUserModel::LoadMotion hands
  // back an instance it does not keep.
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
  m_canvas = {};
  m_physics = false;
  m_pose = false;
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

  const auto manifest = nx::vfs::read(model3_path);
  if (!manifest) {
    error = nx::format("no model at '{}'", model3_path);
    return false;
  }

  csm::CubismModelSettingJson settings(
      const_cast<csm::csmByte *>(manifest->data()),
      nx::cast<csm::csmSizeInt>(manifest->size()));

  const char *const moc_name = settings.GetModelFileName();
  if (empty_name(moc_name)) {
    error = nx::format("{}: names no .moc3", model3_path);
    return false;
  }
  const nx::string moc_path = beside(model3_path, moc_name);
  const auto moc = nx::vfs::read(moc_path);
  if (!moc) {
    error = nx::format("no moc at '{}'", moc_path);
    return false;
  }

  const u32 version = nx::cast<u32>(
      core::csmGetMocVersion(moc->data(), nx::cast<unsigned int>(moc->size())));
  if (version == 0 || version > latest_moc_version()) {
    error = nx::format("{}: moc3 format {} and this Core reads up to {}",
                       moc_path, version, latest_moc_version());
    return false;
  }

  auto *const owner = CSM_NEW HostedModel();
  owner->LoadModel(moc->data(), nx::cast<csm::csmSizeInt>(moc->size()), true);
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
    path_out = beside(model3_path, name);
    auto bytes = nx::vfs::read(path_out);
    if (!bytes) {
      out.m_missing.push_back(path_out);
      nx::logw("live2d: {} names '{}', which is not there", model3_path, name);
    }
    return bytes;
  };

  out.m_textures.reserve(nx::cast<usize>(settings.GetTextureCount()));
  for (i32 i = 0; i < settings.GetTextureCount(); ++i) {
    const char *const name = settings.GetTextureFileName(i);
    if (empty_name(name)) {
      out.m_textures.push_back(pack_texture(NX_TEXTURE_NONE, 0));
      continue;
    }
    const nx::string path = beside(model3_path, name);
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
      owner->LoadPhysics(bytes->data(),
                         nx::cast<csm::csmSizeInt>(bytes->size()));
      out.m_physics = owner->_physics != nullptr;
    }

  if (const char *const name = settings.GetPoseFileName(); !empty_name(name))
    if (const auto bytes = read_optional(name, path)) {
      owner->LoadPose(bytes->data(), nx::cast<csm::csmSizeInt>(bytes->size()));
      out.m_pose = owner->_pose != nullptr;
    }

  if (const char *const name = settings.GetUserDataFile(); !empty_name(name))
    if (const auto bytes = read_optional(name, path))
      owner->LoadUserData(bytes->data(),
                          nx::cast<csm::csmSizeInt>(bytes->size()));

  for (i32 i = 0; i < settings.GetExpressionCount(); ++i) {
    const char *const file = settings.GetExpressionFileName(i);
    if (empty_name(file))
      continue;
    const auto bytes = read_optional(file, path);
    if (!bytes)
      continue;
    const char *const name = settings.GetExpressionName(i);
    csm::ACubismMotion *const motion = owner->LoadExpression(
        bytes->data(), nx::cast<csm::csmSizeInt>(bytes->size()), name);
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

      const nx::string name = nx::format("{}_{}", group, i);
      csm::ACubismMotion *const motion = owner->LoadMotion(
          bytes->data(), nx::cast<csm::csmSizeInt>(bytes->size()), name.c_str(),
          nullptr, nullptr, &settings, group, i);
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

  // The manifest's own parameter groups. Both are optional and plenty of
  // models in the wild have neither, which is why nothing here fails without.
  owner->_eyeBlink = csm::CubismEyeBlink::Create(&settings);
  out.m_eye_blink = owner->_eyeBlink != nullptr;

  for (i32 i = 0; i < settings.GetLipSyncParameterCount(); ++i)
    if (const csm::CubismIdHandle id = settings.GetLipSyncParameterId(i);
        id != nullptr)
      out.m_lip_sync.push_back(nx::string(id->GetString().GetRawString()));

  // Cubism's own idle sway, on the standard parameters. A model whose rig does
  // not have them simply ignores the writes.
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
           model3_path, out.parameter_count(), out.part_count(),
           out.drawable_count(), out.motions().size(), out.expressions().size(),
           out.has_physics() ? ", physics" : "",
           out.has_pose() ? ", pose" : "");
  if (!out.m_missing.empty())
    nx::logw("live2d: '{}' names {} file(s) that are not there", model3_path,
             out.m_missing.size());
  return true;
}

} // namespace nxm::live2d
