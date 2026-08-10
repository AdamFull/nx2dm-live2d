#include "live2d/live2d_draw.h"

#include "core/foundation/diagnostics/log.h"
#include "core/rendering/render2d/render_interop.h"
#include "core/rendering/render2d/scene_renderer.h"

#include <Model/CubismModel.hpp>
#include <Rendering/csmBlendMode.hpp>

#include <algorithm>
#include <cmath>

namespace nxm::live2d {
namespace {

namespace csm = Live2D::Cubism::Framework;
namespace r2d = nxe::r2d;

namespace core = Live2D::Cubism::Core;

[[nodiscard]] r2d::MeshBlend blend_of(const i32 color_blend,
                                      bool &unsupported) noexcept {
  switch (color_blend) {
  case core::csmColorBlendType_Normal:
    return r2d::MeshBlend::Normal;
  case core::csmColorBlendType_AddCompatible:
  case core::csmColorBlendType_Add:
    return r2d::MeshBlend::Additive;
  case core::csmColorBlendType_MultiplyCompatible:
  case core::csmColorBlendType_Multiply:
    return r2d::MeshBlend::Multiply;
  case core::csmColorBlendType_Screen:
    return r2d::MeshBlend::Screen;
  default:
    break;
  }
  unsupported = true;
  return r2d::MeshBlend::Normal;
}

/// Cubism has no per-vertex colour: a drawable's opacity and its multiply
/// colour apply to the whole of it, so one packed word serves every vertex.
[[nodiscard]] u32 tint(const core::csmVector4 &multiply, const f32 opacity,
                       const glm::vec4 &view) noexcept {
  return pack_color(glm::vec4(multiply.X * view.x, multiply.Y * view.y,
                              multiply.Z * view.z, opacity * view.w));
}

/// A screen colour of black is the identity, which is what an untouched
/// drawable carries. Anything else needs the fragment shader L4 brings.
[[nodiscard]] bool uses_screen_colour(const core::csmVector4 &screen) noexcept {
  constexpr f32 EPSILON = 1.f / 512.f;
  return screen.X > EPSILON || screen.Y > EPSILON || screen.Z > EPSILON;
}

[[nodiscard]] glm::vec2 flip_v(const glm::vec2 uv) noexcept {
  return {uv.x, 1.f - uv.y};
}

static_assert(sizeof(core::csmVector2) == sizeof(glm::vec2));
static_assert(alignof(core::csmVector2) == alignof(glm::vec2));

/// The 2D affine part of one of the layout's 4x4s, as a mat3 whose third
/// column is the translation. Those matrices only translate and scale in xy;
/// carrying the rest of a mat4 to the GPU would cost half the push range.
[[nodiscard]] glm::mat3 affine_of(const glm::mat4 &m) noexcept {
  glm::mat3 out(1.f);
  out[0] = glm::vec3(m[0][0], m[0][1], 0.f);
  out[1] = glm::vec3(m[1][0], m[1][1], 0.f);
  out[2] = glm::vec3(m[3][0], m[3][1], 1.f);
  return out;
}

/// Inverse of a 2D affine held as a mat3. False when it is singular: a model
/// scaled to nothing has no inverse, and the infinities would reach the shader
/// as a mask sampled at nowhere.
[[nodiscard]] bool invert_affine(const glm::mat3 &m, glm::mat3 &out) noexcept {
  const f32 a = m[0][0];
  const f32 b = m[1][0];
  const f32 c = m[0][1];
  const f32 d = m[1][1];
  const f32 det = a * d - b * c;
  if (std::fabs(det) < 1e-12f)
    return false;

  const f32 inv = 1.f / det;
  out = glm::mat3(1.f);
  out[0] = glm::vec3(d * inv, -c * inv, 0.f);
  out[1] = glm::vec3(-b * inv, a * inv, 0.f);
  out[2] = glm::vec3(-(out[0][0] * m[2][0] + out[1][0] * m[2][1]),
                     -(out[0][1] * m[2][0] + out[1][1] * m[2][1]), 1.f);
  return true;
}

[[nodiscard]] DrawMask clip_of(const MaskLayout &masks, const i32 drawable,
                               const glm::mat3 &inverse_world) noexcept {
  const MaskRef &ref = masks.of(drawable);
  if (!ref.clipped())
    return {};
  return {
      .group = ref.group,
      .channel = ref.channel,
      .inverted = ref.inverted,
      // The vertices went out in world space, so the shader has to come back
      // to model space before the layout's matrix means anything.
      .from_world = affine_of(ref.to_mask) * inverse_world,
  };
}

} // namespace

DrawableMesh drawable_mesh(const ModelAsset &asset,
                           const i32 drawable) noexcept {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr || drawable < 0)
    return {};
  auto *const m = const_cast<csm::CubismModel *>(model);
  if (drawable >= m->GetDrawableCount())
    return {};

  const i32 vertices = m->GetDrawableVertexCount(drawable);
  const i32 indices = m->GetDrawableVertexIndexCount(drawable);
  const core::csmVector2 *const points =
      m->GetDrawableVertexPositions(drawable);
  const core::csmVector2 *const uvs = m->GetDrawableVertexUvs(drawable);
  const csm::csmUint16 *const source = m->GetDrawableVertexIndices(drawable);
  if (vertices <= 0 || indices <= 0 || points == nullptr || uvs == nullptr ||
      source == nullptr)
    return {};

  const usize count = nx::cast<usize>(vertices);
  return {
      {reinterpret_cast<const glm::vec2 *>(points), count},
      {reinterpret_cast<const glm::vec2 *>(uvs), count},
      {source, nx::cast<usize>(indices)},
  };
}

bool drawable_visible(const ModelAsset &asset, const i32 drawable) noexcept {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr || drawable < 0)
    return false;
  auto *const m = const_cast<csm::CubismModel *>(model);
  return drawable < m->GetDrawableCount() &&
         m->GetDrawableDynamicFlagIsVisible(drawable);
}

usize masked_drawable_count(const ModelAsset &asset) noexcept {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return 0u;
  auto *const m = const_cast<csm::CubismModel *>(model);

  const i32 *const counts = m->GetDrawableMaskCounts();
  if (counts == nullptr)
    return 0u;

  usize masked = 0;
  for (i32 d = 0; d < m->GetDrawableCount(); ++d)
    if (counts[d] > 0 && m->GetDrawableDynamicFlagIsVisible(d))
      ++masked;
  return masked;
}

usize emit_masks(const ModelAsset &asset, const MaskLayout &masks,
                 MaskChannel &out) {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return 0u;
  auto *const m = const_cast<csm::CubismModel *>(model);
  const std::span<const u32> textures = asset.textures();

  nx::vector<r2d::MeshVertex> vertices;
  nx::vector<u32> indices;
  usize appended = 0;

  for (usize g = 0; g < masks.groups().size(); ++g) {
    const MaskGroup &group = masks.groups()[g];
    for (const i32 shape : group.shapes) {
      const DrawableMesh mesh = drawable_mesh(asset, shape);
      if (!mesh.valid())
        continue;
      // Opaque, and the mask shader does not read it. Deliberately not the
      // shape's own opacity: a mask shape is usually an invisible helper - two
      // of the development model's six sit at zero - so weighting coverage by
      // it empties their masks and clips everything using them out of
      // existence. Cubism's own mask shader is channelFlag * texture.a.
      const u32 color = pack_color(glm::vec4(1.f, 1.f, 1.f, 1.f));

      vertices.clear();
      vertices.reserve(mesh.positions.size());
      for (usize v = 0; v < mesh.positions.size(); ++v)
        vertices.push_back({mesh.positions[v], flip_v(mesh.uvs[v]), color});

      indices.clear();
      indices.reserve(mesh.indices.size());
      for (const u16 index : mesh.indices)
        indices.push_back(nx::cast<u32>(index));

      MaskDraw draw;
      draw.first_index = nx::cast<u32>(out.indices.size());
      draw.index_count = nx::cast<u32>(indices.size());
      draw.vertex_offset = nx::cast<u32>(out.vertices.size());
      const i32 page = m->GetDrawableTextureIndex(shape);
      draw.texture = page >= 0 && nx::cast<usize>(page) < textures.size()
                         ? textures[nx::cast<usize>(page)]
                         : pack_texture(NX_TEXTURE_NONE, 0);
      draw.atlas = group.atlas;
      draw.channel = group.channel;
      draw.to_mask = affine_of(group.to_mask);
      draw.tile = group.tile;

      out.vertices.insert(out.vertices.end(), vertices.begin(), vertices.end());
      out.indices.insert(out.indices.end(), indices.begin(), indices.end());
      out.draws.push_back(draw);
      ++appended;
    }
  }
  return appended;
}

namespace {

usize emit(const ModelAsset &asset, const ModelView &view,
           r2d::MeshChannel &out, const MaskLayout *const masks,
           nx::vector<DrawMask> *const out_masks) {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return 0u;
  auto *const m = const_cast<csm::CubismModel *>(model);

  const i32 *const order = m->GetRenderOrders();
  const i32 *const mask_counts = m->GetDrawableMaskCounts();
  const i32 count = m->GetDrawableCount();
  if (order == nullptr || count <= 0)
    return 0u;

  // Cubism's render order is a rank per drawable, so it is turned round into a
  // list of drawables in the order they are drawn.
  nx::vector<i32> sorted;
  sorted.reserve(nx::cast<usize>(count));
  for (i32 d = 0; d < count; ++d)
    sorted.push_back(d);
  std::stable_sort(
      sorted.begin(), sorted.end(),
      [order](const i32 a, const i32 b) { return order[a] < order[b]; });

  const u32 layer = nx::cast<u32>(view.layer + 2048) & 0xFFFu;
  const u32 key = nx_make_sort_key(
      layer,
      r2d::quantize_depth(view.world[2][1], view.depth_min, view.depth_max),
      0u);
  const f32 model_opacity = m->GetModelOpacity();
  const std::span<const u32> textures = asset.textures();

  glm::mat3 inverse_world(1.f);
  const bool can_clip =
      masks != nullptr && invert_affine(view.world, inverse_world);
  if (masks != nullptr && !can_clip)
    nx::logw("live2d: the model's placement is singular; nothing can be "
             "clipped through it");

  nx::vector<r2d::MeshVertex> vertices;
  nx::vector<u32> indices;
  usize appended = 0;
  usize screen_coloured = 0;
  usize exotic_blends = 0;

  for (const i32 d : sorted) {
    if (!m->GetDrawableDynamicFlagIsVisible(d))
      continue;
    const DrawableMesh mesh = drawable_mesh(asset, d);
    if (!mesh.valid())
      continue;

    if (uses_screen_colour(m->GetDrawableScreenColor(d)))
      ++screen_coloured;

    const u32 color =
        tint(m->GetDrawableMultiplyColor(d),
             m->GetDrawableOpacity(d) * model_opacity, view.color);

    vertices.clear();
    vertices.reserve(mesh.positions.size());
    for (usize v = 0; v < mesh.positions.size(); ++v) {
      const f32 x = mesh.positions[v].x;
      const f32 y = mesh.positions[v].y;
      r2d::MeshVertex vertex;
      vertex.position = glm::vec2(
          view.world[0][0] * x + view.world[1][0] * y + view.world[2][0],
          view.world[0][1] * x + view.world[1][1] * y + view.world[2][1]);
      vertex.uv = flip_v(mesh.uvs[v]);
      vertex.color = color;
      vertices.push_back(vertex);
    }

    indices.clear();
    indices.reserve(mesh.indices.size());
    for (const u16 index : mesh.indices)
      indices.push_back(nx::cast<u32>(index));

    bool exotic = false;
    csm::csmBlendMode mode = m->GetDrawableBlendModeType(d);
    const r2d::MeshBlend blend = blend_of(mode.GetColorBlendType(), exotic);
    exotic_blends += exotic ? 1u : 0u;

    const i32 page = m->GetDrawableTextureIndex(d);
    r2d::MeshDraw &draw = out.append(vertices, indices);
    draw.texture = page >= 0 && nx::cast<usize>(page) < textures.size()
                       ? textures[nx::cast<usize>(page)]
                       : pack_texture(NX_TEXTURE_NONE, 0);
    draw.sort_key = key;
    draw.camera = view.camera;
    draw.blend = blend;
    ++appended;

    if (out_masks != nullptr)
      out_masks->push_back(can_clip ? clip_of(*masks, d, inverse_world)
                                    : DrawMask{});
  }

  // Once per model rather than per drawable, and at all rather than never: a
  // face drawn without its clip is a visible defect and the log is where
  // someone looking at one starts.
  if (const usize masked = masks == nullptr && mask_counts != nullptr
                               ? masked_drawable_count(asset)
                               : 0u;
      masked > 0)
    nx::logw("live2d: {} of {} drawables are clipped and this path does not "
             "mask; they will draw whole",
             masked, appended);
  if (screen_coloured > 0)
    nx::logw("live2d: {} drawables use a screen colour, which this path "
             "ignores",
             screen_coloured);
  if (exotic_blends > 0)
    nx::logw("live2d: {} drawables use a blend mode beyond normal, additive, "
             "multiply and screen; they draw normally",
             exotic_blends);
  return appended;
}

} // namespace

usize emit_model(const ModelAsset &asset, const ModelView &view,
                 r2d::MeshChannel &out) {
  return emit(asset, view, out, nullptr, nullptr);
}

usize emit_model(const ModelAsset &asset, const ModelView &view,
                 const MaskLayout &masks, r2d::MeshChannel &out,
                 nx::vector<DrawMask> &out_masks) {
  return emit(asset, view, out, &masks, &out_masks);
}

} // namespace nxm::live2d
