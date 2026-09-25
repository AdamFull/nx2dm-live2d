#include "live2d/live2d_assets.h"

#include "live2d/live2d_asset_bundle.h"
#include "live2d/live2d_platform.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/serialization/asset_policy.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/vfs.h"
#include "rendering/render2d/render_interop.h"

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

  // LoadModel with a moc it does not own, which it leaves to release() rather
  // than the base destructor.
  [[nodiscard]] bool adopt(SharedMoc &shared) {
    _moc = shared.moc();
    _model = shared.create_model();
    if (_model == nullptr)
      return false;
    _model->SaveParameters();
    _modelMatrix = CSM_NEW csm::CubismModelMatrix(_model->GetCanvasWidth(),
                                                  _model->GetCanvasHeight());
    return true;
  }

  void release(SharedMoc &shared) noexcept {
    shared.delete_model(_model);
    _model = nullptr;
    _moc = nullptr;
  }
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

class VfsReader final : public ModelReader {
public:
  [[nodiscard]] nx::vfs::FileInfo stat(const nx::string_view path) override {
    return nx::vfs::stat(path);
  }
  [[nodiscard]] std::optional<nx::blob<u8>> read(const nx::string_view path,
                                                 const u64 max_bytes) override {
    auto bytes = nx::vfs::read(path);
    if (!bytes || bytes->size() > max_bytes)
      return std::nullopt;
    return std::move(bytes.value());
  }
};

[[nodiscard]] nx::shared_ptr<const ModelMesh>
build_mesh(csm::CubismModel &model) {
  nx::shared_ptr<ModelMesh> mesh = nx::make_shared<ModelMesh>();
  const i32 count = nx::max(model.GetDrawableCount(), 0);
  mesh->first_vertex.reserve(nx::cast<usize>(count) + 1u);
  mesh->first_index.reserve(nx::cast<usize>(count) + 1u);
  mesh->first_vertex.push_back(0u);
  mesh->first_index.push_back(0u);
  for (i32 d = 0; d < count; ++d) {
    const core::csmVector2 *const uvs = model.GetDrawableVertexUvs(d);
    const csm::csmUint16 *const indices = model.GetDrawableVertexIndices(d);
    const i32 vertices = uvs != nullptr ? model.GetDrawableVertexCount(d) : 0;
    const i32 triangles =
        indices != nullptr ? model.GetDrawableVertexIndexCount(d) : 0;
    for (i32 v = 0; v < vertices; ++v)
      mesh->uvs.push_back({uvs[v].X, 1.f - uvs[v].Y});
    for (i32 i = 0; i < triangles; ++i)
      mesh->indices.push_back(indices[i]);
    mesh->first_vertex.push_back(nx::cast<u32>(mesh->uvs.size()));
    mesh->first_index.push_back(nx::cast<u32>(mesh->indices.size()));
  }
  return mesh;
}

// Models load on several workers at once. Core is not safe to revive mocs or
// create models concurrently: on arm64 it called a null pointer inside
// csmInitializeModelInPlace. The moc also counts its models in a plain integer.
nx::mutex &core_lock() noexcept {
  static nx::mutex lock;
  return lock;
}

} // namespace

SharedMoc::~SharedMoc() { csm::CubismMoc::Delete(m_moc); }

nx::shared_ptr<SharedMoc> SharedMoc::revive(const std::span<const u8> bytes) {
  if (bytes.empty() ||
      bytes.size() >
          nx::cast<usize>(std::numeric_limits<csm::csmSizeInt>::max()))
    return {};
  csm::CubismMoc *moc = nullptr;
  {
    const nx::scoped_lock<nx::mutex> held(core_lock());
    moc = csm::CubismMoc::Create(bytes.data(),
                                 nx::cast<csm::csmSizeInt>(bytes.size()), true);
  }
  if (moc == nullptr)
    return {};
  nx::shared_ptr<SharedMoc> shared = nx::make_shared<SharedMoc>(moc);
  csm::CubismModel *const probe = shared->create_model();
  if (probe == nullptr)
    return {};
  shared->m_mesh = build_mesh(*probe);
  shared->delete_model(probe);
  return shared;
}

csm::CubismModel *SharedMoc::create_model() {
  const nx::scoped_lock<nx::mutex> held(core_lock());
  return m_moc->CreateModel();
}

void SharedMoc::delete_model(csm::CubismModel *const model) noexcept {
  if (model == nullptr)
    return;
  const nx::scoped_lock<nx::mutex> held(core_lock());
  m_moc->DeleteModel(model);
}

nx::shared_ptr<SharedMoc> MocCache::find(const nx::string_view key,
                                         const u64 generation) const {
  const nx::scoped_lock<nx::mutex> held(m_lock);
  for (const Entry &entry : m_entries)
    if (entry.generation == generation && entry.key == key)
      return entry.moc;
  return {};
}

nx::shared_ptr<SharedMoc> MocCache::add(const nx::string_view key,
                                        const u64 generation,
                                        nx::shared_ptr<SharedMoc> moc) {
  const nx::scoped_lock<nx::mutex> held(m_lock);
  for (Entry &entry : m_entries)
    if (entry.key == key) {
      if (entry.generation != generation) {
        entry.generation = generation;
        entry.moc = std::move(moc);
      }
      return entry.moc;
    }
  m_entries.push_back({nx::string(key), generation, moc});
  return moc;
}

usize MocCache::prune() {
  const nx::scoped_lock<nx::mutex> held(m_lock);
  usize dropped = 0;
  for (usize i = 0; i < m_entries.size();) {
    if (m_entries[i].moc.use_count() > 1u) {
      ++i;
      continue;
    }
    m_entries[i] = std::move(m_entries.back());
    m_entries.pop_back();
    ++dropped;
  }
  return dropped;
}

void MocCache::clear() noexcept {
  const nx::scoped_lock<nx::mutex> held(m_lock);
  m_entries.clear();
}

usize MocCache::size() const noexcept {
  const nx::scoped_lock<nx::mutex> held(m_lock);
  return m_entries.size();
}

ModelAsset::~ModelAsset() { reset(); }

ModelAsset::ModelAsset(ModelAsset &&other) noexcept
    : m_owner(other.m_owner), m_textures(std::move(other.m_textures)),
      m_texture_paths(std::move(other.m_texture_paths)),
      m_motions(std::move(other.m_motions)),
      m_expressions(std::move(other.m_expressions)),
      m_missing(std::move(other.m_missing)),
      m_lip_sync(std::move(other.m_lip_sync)),
      m_dependencies(std::move(other.m_dependencies)), m_canvas(other.m_canvas),
      m_moc(std::move(other.m_moc)), m_physics(other.m_physics),
      m_pose(other.m_pose), m_eye_blink(other.m_eye_blink) {
  other.m_owner = nullptr;
}

ModelAsset &ModelAsset::operator=(ModelAsset &&other) noexcept {
  if (this != &other) {
    reset();
    m_owner = other.m_owner;
    m_textures = std::move(other.m_textures);
    m_texture_paths = std::move(other.m_texture_paths);
    m_motions = std::move(other.m_motions);
    m_expressions = std::move(other.m_expressions);
    m_missing = std::move(other.m_missing);
    m_lip_sync = std::move(other.m_lip_sync);
    m_dependencies = std::move(other.m_dependencies);
    m_canvas = other.m_canvas;
    m_moc = std::move(other.m_moc);
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
    auto *const hosted = static_cast<HostedModel *>(m_owner);
    if (m_moc)
      hosted->release(*m_moc);
    CSM_DELETE(hosted);
    m_owner = nullptr;
  }
  m_textures.clear();
  m_texture_paths.clear();
  m_missing.clear();
  m_lip_sync.clear();
  m_dependencies.clear();
  m_canvas = {};
  m_moc = {};
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

bool gather_model(const nx::string_view model3_path, ModelReader &reader,
                  ModelSource &out, nx::string &error) {
  out = {};
  error.clear();
  const bool explicit_cooked = ends_with(model3_path, ".nxb");
  out.authored_path =
      explicit_cooked
          ? nx::string(model3_path.substr(0, model3_path.size() - 4))
          : nx::string(model3_path);
  out.cooked_path = nx::string(model3_path);
  if (!explicit_cooked)
    out.cooked_path += ".nxb";

  const nx::vfs::FileInfo cooked_info = reader.stat(out.cooked_path.view());
  if (cooked_info.exists) {
    if (cooked_info.is_directory ||
        cooked_info.size > MAX_LIVE2D_BUNDLE_BYTES) {
      error = nx::format("cooked model '{}' exceeds its size limit",
                         out.cooked_path);
      return false;
    }
    // A read that fails leaves the bundle empty, and build_model reports it
    // malformed.
    out.bundled = true;
    if (auto bytes =
            reader.read(out.cooked_path.view(), MAX_LIVE2D_BUNDLE_BYTES))
      out.cooked = std::move(bytes.value());
    out.cooked_generation = nx::vfs::file_generation(out.cooked_path.view());
    return true;
  }
  if (explicit_cooked ||
      !nx::asset_policy::can_fallback_to_authored_source(cooked_info.exists)) {
    error = nx::format("no cooked model at '{}'", out.cooked_path);
    return false;
  }

  auto bytes = reader.read(out.authored_path.view(), MAX_LIVE2D_MANIFEST_BYTES);
  if (!bytes || bytes->empty()) {
    error = nx::format("no bounded model at '{}'", out.authored_path);
    return false;
  }
  out.manifest = std::move(bytes.value());
  ModelManifest manifest;
  if (!parse_model_manifest(
          nx::string_view(reinterpret_cast<const char *>(out.manifest.data()),
                          out.manifest.size()),
          manifest, error)) {
    error = nx::format("{}: {}", out.authored_path, error);
    return false;
  }
  for (const nx::string &name : manifest.embedded) {
    if (!nx::resource_bundle::valid_name(name.view()))
      continue;
    const nx::string path = beside(out.authored_path.view(), name.view());
    auto file = reader.read(path.view(), MAX_LIVE2D_RESOURCE_BYTES);
    if (!file || file->empty())
      continue;
    out.files.push_back(
        {name, std::move(file.value()), nx::vfs::file_generation(path.view())});
  }
  return true;
}

usize resolve_textures(ModelAsset &asset, const TextureResolver &resolve) {
  usize changed = 0;
  for (usize i = 0;
       i < asset.m_texture_paths.size() && i < asset.m_textures.size(); ++i) {
    const nx::string &path = asset.m_texture_paths[i];
    if (path.empty())
      continue;
    const u32 packed =
        resolve ? resolve(path.view()) : pack_texture(NX_TEXTURE_NONE, 0);
    changed += asset.m_textures[i] != packed ? 1u : 0u;
    asset.m_textures[i] = packed;
  }
  return changed;
}

bool load_model(const nx::string_view model3_path, TextureResolver resolve,
                ModelAsset &out, nx::string &error, MocCache *const mocs) {
  VfsReader reader;
  ModelSource source;
  if (!gather_model(model3_path, reader, source, error)) {
    out = ModelAsset();
    return false;
  }
  if (!build_model(source, out, error, mocs))
    return false;
  (void)resolve_textures(out, resolve);
  return true;
}

bool build_model(const ModelSource &source, ModelAsset &out, nx::string &error,
                 MocCache *const mocs) {
  if (!install_platform()) {
    error = "the Cubism framework would not start";
    return false;
  }
  out.reset();
  error.clear();

  const nx::string &authored_path = source.authored_path;
  const nx::string &cooked_path = source.cooked_path;
  std::optional<ModelBundleView> bundle;
  std::span<const u8> manifest;
  out.m_dependencies.push_back(cooked_path);
  if (source.bundled) {
    bundle = open_model_bundle({source.cooked.data(), source.cooked.size()});
    if (!bundle) {
      error = nx::format("cooked model '{}' is malformed", cooked_path);
      return false;
    }
    manifest = bundle->model_json();
  } else {
    out.m_dependencies.push_back(authored_path);
    manifest = {source.manifest.data(), source.manifest.size()};
  }

  const auto find = [&](const nx::string_view name) -> LoadedResource {
    if (!nx::resource_bundle::valid_name(name))
      return {};
    if (bundle)
      return {.storage = {}, .bytes = bundle->resources.find(name)};
    for (const ModelSource::File &file : source.files)
      if (file.name.view() == name)
        return {.storage = {}, .bytes = {file.bytes.data(), file.bytes.size()}};
    return {};
  };
  const auto generation_of = [&](const nx::string_view name) -> u64 {
    if (bundle)
      return source.cooked_generation;
    for (const ModelSource::File &file : source.files)
      if (file.name.view() == name)
        return file.generation;
    return 0;
  };

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
  // A bundle's moc changes with the bundle; an authored one with its file.
  const nx::string moc_key =
      bundle ? nx::format("{}#{}", cooked_path, moc_name) : moc_path;
  const u64 moc_generation = generation_of(moc_name);
  nx::shared_ptr<SharedMoc> shared =
      mocs != nullptr ? mocs->find(moc_key.view(), moc_generation)
                      : nx::shared_ptr<SharedMoc>{};
  if (!shared) {
    const LoadedResource moc = find(moc_name);
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

    shared = SharedMoc::revive(moc.bytes);
    if (!shared) {
      error = nx::format("{}: Core refused the moc", moc_path);
      return false;
    }
    if (mocs != nullptr)
      shared = mocs->add(moc_key.view(), moc_generation, std::move(shared));
  }

  auto *const owner = CSM_NEW HostedModel();
  if (!owner->adopt(*shared)) {
    owner->release(*shared);
    CSM_DELETE(owner);
    error = nx::format("{}: Core refused the moc", moc_path);
    return false;
  }
  out.m_owner = owner;
  out.m_moc = std::move(shared);

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
    LoadedResource bytes = find(name);
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
      out.m_texture_paths.emplace_back();
      out.m_textures.push_back(pack_texture(NX_TEXTURE_NONE, 0));
      continue;
    }
    const nx::string path = beside(authored_path.view(), name);
    out.m_dependencies.push_back(path);
    out.m_texture_paths.push_back(path);
    out.m_textures.push_back(pack_texture(NX_TEXTURE_NONE, 0));
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
  nx::logd("live2d: '{}' - {} parameters, {} parts, {} drawables, {} motions, "
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
