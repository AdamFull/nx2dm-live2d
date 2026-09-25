#pragma once

#include "core/foundation/core/callable.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/vfs/vfs.h"

#include <glm/vec2.hpp>

#include <optional>
#include <span>

namespace Live2D::Cubism::Framework {
class CubismMoc;
class CubismModel;
class CubismUserModel;
class ACubismMotion;
} // namespace Live2D::Cubism::Framework

namespace nxm::live2d {

using TextureResolver = nx::function<u32(nx::string_view path)>;

struct MotionEntry {
  nx::string group;
  i32 index = 0;
  Live2D::Cubism::Framework::ACubismMotion *motion = nullptr;
};

struct ExpressionEntry {
  nx::string name;
  Live2D::Cubism::Framework::ACubismMotion *motion = nullptr;
};

struct CanvasInfo {
  f32 width = 0.f;
  f32 height = 0.f;
  f32 origin_x = 0.f;
  f32 origin_y = 0.f;
  f32 pixels_per_unit = 1.f;

  [[nodiscard]] f32 width_units() const noexcept {
    return pixels_per_unit > 0.f ? width / pixels_per_unit : 0.f;
  }
  [[nodiscard]] f32 height_units() const noexcept {
    return pixels_per_unit > 0.f ? height / pixels_per_unit : 0.f;
  }
  [[nodiscard]] f32 origin_x_units() const noexcept {
    return pixels_per_unit > 0.f ? origin_x / pixels_per_unit : 0.f;
  }
  [[nodiscard]] f32 origin_y_units() const noexcept {
    return pixels_per_unit > 0.f ? origin_y / pixels_per_unit : 0.f;
  }
};

// What Cubism never changes after load: every drawable's texture coordinates
// (v = 0 at the top) and triangle indices, end to end. A drawable's indices
// count from its own first vertex.
struct ModelMesh {
  nx::vector<glm::vec2> uvs;
  nx::vector<u32> indices;
  nx::vector<u32> first_vertex;
  nx::vector<u32> first_index;

  [[nodiscard]] u32 vertex_count() const noexcept {
    return first_vertex.empty() ? 0u : first_vertex.back();
  }
  [[nodiscard]] u32 drawable_count() const noexcept {
    return first_vertex.empty() ? 0u : nx::cast<u32>(first_vertex.size() - 1u);
  }
};

/// A moc revived once and shared by every model made from it, with the mesh
/// its drawables never change.
class SharedMoc {
public:
  explicit SharedMoc(Live2D::Cubism::Framework::CubismMoc *moc) noexcept
      : m_moc(moc) {}
  ~SharedMoc();

  SharedMoc(const SharedMoc &) = delete;
  SharedMoc &operator=(const SharedMoc &) = delete;

  /// Null when Core refuses the bytes, or they fail its consistency check.
  [[nodiscard]] static nx::shared_ptr<SharedMoc>
  revive(std::span<const u8> bytes);

  [[nodiscard]] Live2D::Cubism::Framework::CubismMoc *moc() const noexcept {
    return m_moc;
  }
  [[nodiscard]] const nx::shared_ptr<const ModelMesh> &mesh() const noexcept {
    return m_mesh;
  }

  // The moc counts its models in a plain integer.
  [[nodiscard]] Live2D::Cubism::Framework::CubismModel *create_model();
  void delete_model(Live2D::Cubism::Framework::CubismModel *model) noexcept;

private:
  Live2D::Cubism::Framework::CubismMoc *m_moc = nullptr;
  nx::shared_ptr<const ModelMesh> m_mesh;
  nx::mutex m_models;
};

/// Mocs by file and generation, so models made from one file share it. Models
/// may load on several threads at once.
class MocCache {
public:
  [[nodiscard]] nx::shared_ptr<SharedMoc> find(nx::string_view key,
                                               u64 generation) const;
  /// Returns the moc to use: one that another load of the same generation
  /// added first, or @p moc.
  [[nodiscard]] nx::shared_ptr<SharedMoc>
  add(nx::string_view key, u64 generation, nx::shared_ptr<SharedMoc> moc);

  /// Drops the mocs no model holds any longer. Returns how many.
  usize prune();
  void clear() noexcept;

  [[nodiscard]] usize size() const noexcept;

private:
  struct Entry {
    nx::string key;
    u64 generation = 0;
    nx::shared_ptr<SharedMoc> moc;
  };
  mutable nx::mutex m_lock;
  nx::vector<Entry> m_entries;
};

class ModelAsset {
public:
  ModelAsset() = default;
  ~ModelAsset();

  ModelAsset(const ModelAsset &) = delete;
  ModelAsset &operator=(const ModelAsset &) = delete;
  ModelAsset(ModelAsset &&other) noexcept;
  ModelAsset &operator=(ModelAsset &&other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return m_owner != nullptr; }

  [[nodiscard]] Live2D::Cubism::Framework::CubismUserModel *
  owner() const noexcept {
    return m_owner;
  }
  [[nodiscard]] Live2D::Cubism::Framework::CubismModel *model() const noexcept;

  [[nodiscard]] usize parameter_count() const noexcept;
  [[nodiscard]] usize part_count() const noexcept;
  [[nodiscard]] usize drawable_count() const noexcept;

  [[nodiscard]] CanvasInfo canvas() const noexcept { return m_canvas; }

  [[nodiscard]] const nx::shared_ptr<const ModelMesh> &mesh() const noexcept {
    static const nx::shared_ptr<const ModelMesh> none;
    return m_moc ? m_moc->mesh() : none;
  }
  [[nodiscard]] const nx::shared_ptr<SharedMoc> &moc() const noexcept {
    return m_moc;
  }

  [[nodiscard]] std::span<const u32> textures() const noexcept {
    return {m_textures.data(), m_textures.size()};
  }

  [[nodiscard]] std::span<const MotionEntry> motions() const noexcept {
    return {m_motions.data(), m_motions.size()};
  }
  [[nodiscard]] std::span<const ExpressionEntry> expressions() const noexcept {
    return {m_expressions.data(), m_expressions.size()};
  }

  [[nodiscard]] Live2D::Cubism::Framework::ACubismMotion *
  find_motion(nx::string_view group, i32 index) const noexcept;
  [[nodiscard]] Live2D::Cubism::Framework::ACubismMotion *
  find_expression(nx::string_view name) const noexcept;

  [[nodiscard]] bool has_physics() const noexcept { return m_physics; }
  [[nodiscard]] bool has_pose() const noexcept { return m_pose; }
  [[nodiscard]] bool has_eye_blink() const noexcept { return m_eye_blink; }

  [[nodiscard]] std::span<const nx::string> lip_sync() const noexcept {
    return {m_lip_sync.data(), m_lip_sync.size()};
  }

  [[nodiscard]] std::span<const nx::string> missing() const noexcept {
    return {m_missing.data(), m_missing.size()};
  }
  [[nodiscard]] std::span<const nx::string> dependencies() const noexcept {
    return {m_dependencies.data(), m_dependencies.size()};
  }

private:
  friend bool build_model(const struct ModelSource &, ModelAsset &,
                          nx::string &, MocCache *);
  friend usize resolve_textures(ModelAsset &, const TextureResolver &);

  void reset() noexcept;

  Live2D::Cubism::Framework::CubismUserModel *m_owner = nullptr;
  nx::vector<u32> m_textures;
  nx::vector<nx::string> m_texture_paths;
  nx::vector<MotionEntry> m_motions;
  nx::vector<ExpressionEntry> m_expressions;
  nx::vector<nx::string> m_missing;
  nx::vector<nx::string> m_lip_sync;
  nx::vector<nx::string> m_dependencies;
  CanvasInfo m_canvas;
  nx::shared_ptr<SharedMoc> m_moc;
  bool m_physics = false;
  bool m_pose = false;
  bool m_eye_blink = false;
};

/// Where gather_model reads from: the VFS itself, or an async I/O context on
/// the VFS service.
class ModelReader {
public:
  virtual ~ModelReader() = default;
  [[nodiscard]] virtual nx::vfs::FileInfo stat(nx::string_view path) = 0;
  [[nodiscard]] virtual std::optional<nx::blob<u8>> read(nx::string_view path,
                                                         u64 max_bytes) = 0;
};

/// Every byte a model is built from, read before any of it is parsed: the
/// cooked bundle, or the authored manifest and each file it names.
struct ModelSource {
  struct File {
    nx::string name;
    nx::blob<u8> bytes;
    u64 generation = 0;
  };

  nx::string authored_path;
  nx::string cooked_path;
  bool bundled = false;
  nx::blob<u8> cooked;
  u64 cooked_generation = 0;
  nx::blob<u8> manifest;
  /// The authored files the manifest names; one that is not there is absent.
  nx::vector<File> files;
};

/// Reads everything the model at @p model3_path is built from. An authored
/// tree loads in development; Shipping requires the cooked .model3.json.nxb.
[[nodiscard]] bool gather_model(nx::string_view model3_path,
                                ModelReader &reader, ModelSource &out,
                                nx::string &error);

/// Builds the model from what gather_model read. It does no I/O and touches
/// nothing another thread owns, so it may run on a worker; its textures are
/// left for resolve_textures. With @p mocs, a model whose moc file another
/// model already revived shares that moc and its mesh.
[[nodiscard]] bool build_model(const ModelSource &source, ModelAsset &out,
                               nx::string &error, MocCache *mocs = nullptr);

/// Resolves the model's texture pages, on the thread that owns textures. A
/// page still loading resolves to none; resolving again once it arrives picks
/// it up. Returns how many pages changed.
usize resolve_textures(ModelAsset &asset, const TextureResolver &resolve);

/// gather_model, build_model and resolve_textures on the calling thread, for
/// tools and tests. A running game loads through Live2DSystem instead.
[[nodiscard]] bool load_model(nx::string_view model3_path,
                              TextureResolver resolve, ModelAsset &out,
                              nx::string &error, MocCache *mocs = nullptr);

} // namespace nxm::live2d
