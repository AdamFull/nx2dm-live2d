#include "live2d/live2d_draw.h"

#include "core/foundation/containers/small_vector.h"
#include "core/foundation/diagnostics/log.h"
#include "rendering/render2d/render_interop.h"
#include "rendering/render2d/scene_renderer.h"

#include <Model/CubismModel.hpp>
#include <Rendering/csmBlendMode.hpp>

#include <algorithm>
#include <cstring>

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

[[nodiscard]] u32 tint(const core::csmVector4 &multiply, const f32 opacity,
                       const glm::vec4 &view) noexcept {
  return pack_color(glm::vec4(multiply.X * view.x, multiply.Y * view.y,
                              multiply.Z * view.z, opacity * view.w));
}

[[nodiscard]] bool uses_screen_colour(const core::csmVector4 &screen) noexcept {
  constexpr f32 EPSILON = 1.f / 512.f;
  return screen.X > EPSILON || screen.Y > EPSILON || screen.Z > EPSILON;
}

static_assert(sizeof(core::csmVector2) == sizeof(glm::vec2));
static_assert(alignof(core::csmVector2) == alignof(glm::vec2));

[[nodiscard]] glm::mat3 affine_of(const glm::mat4 &m) noexcept {
  glm::mat3 out(1.f);
  out[0] = glm::vec3(m[0][0], m[0][1], 0.f);
  out[1] = glm::vec3(m[1][0], m[1][1], 0.f);
  out[2] = glm::vec3(m[3][0], m[3][1], 1.f);
  return out;
}
}

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

namespace {

[[nodiscard]] u32 texture_of(const ModelAsset &asset, csm::CubismModel &model,
                             const i32 drawable) {
  const std::span<const u32> textures = asset.textures();
  const i32 page = model.GetDrawableTextureIndex(drawable);
  return page >= 0 && nx::cast<usize>(page) < textures.size()
             ? textures[nx::cast<usize>(page)]
             : pack_texture(NX_TEXTURE_NONE, 0);
}

struct Drawn {
  i32 drawable = 0;
  u32 color = 0;
  u32 texture = 0;
  r2d::MeshBlend blend = r2d::MeshBlend::Normal;
};

// Visits the visible drawables in render order with what every emitter needs.
template <class Visit>
usize walk(const ModelAsset &asset, const ModelView &view, Visit &&visit) {
  const csm::CubismModel *const model = asset.model();
  const ModelMesh *const mesh = asset.mesh().get();
  if (model == nullptr || mesh == nullptr)
    return 0u;
  auto *const m = const_cast<csm::CubismModel *>(model);

  const i32 *const order = m->GetRenderOrders();
  const i32 count =
      nx::min(m->GetDrawableCount(), nx::cast<i32>(mesh->drawable_count()));
  if (order == nullptr || count <= 0)
    return 0u;

  nx::small_vector<i32, 256> sorted;
  sorted.reserve(nx::cast<usize>(count));
  for (i32 d = 0; d < count; ++d)
    sorted.push_back(d);
  std::stable_sort(
      sorted.begin(), sorted.end(),
      [order](const i32 a, const i32 b) { return order[a] < order[b]; });

  const f32 model_opacity = m->GetModelOpacity();
  usize visited = 0;
  usize screen_coloured = 0;
  usize exotic_blends = 0;
  for (const i32 d : sorted) {
    if (!m->GetDrawableDynamicFlagIsVisible(d))
      continue;
    const usize du = nx::cast<usize>(d);
    if (mesh->first_index[du + 1u] == mesh->first_index[du] ||
        mesh->first_vertex[du + 1u] == mesh->first_vertex[du])
      continue;

    if (uses_screen_colour(m->GetDrawableScreenColor(d)))
      ++screen_coloured;
    bool exotic = false;
    csm::csmBlendMode mode = m->GetDrawableBlendModeType(d);
    Drawn drawn;
    drawn.drawable = d;
    drawn.blend = blend_of(mode.GetColorBlendType(), exotic);
    exotic_blends += exotic ? 1u : 0u;
    drawn.color = tint(m->GetDrawableMultiplyColor(d),
                       m->GetDrawableOpacity(d) * model_opacity, view.color);
    drawn.texture = texture_of(asset, *m, d);
    visit(drawn, *mesh);
    ++visited;
  }

  if (screen_coloured > 0)
    nx::logw("live2d: {} drawables use a screen colour, which this path "
             "ignores",
             screen_coloured);
  if (exotic_blends > 0)
    nx::logw("live2d: {} drawables use a blend mode beyond normal, additive, "
             "multiply and screen; they draw normally",
             exotic_blends);
  return visited;
}

} // namespace

usize emit_model(const ModelAsset &asset, const ModelView &view,
                 r2d::MeshChannel &out) {
  const u32 layer = nx::cast<u32>(nx::clamp(view.layer, -32768, 32767) + 32768);
  const u32 key = nx_make_sort_key(
      layer,
      r2d::quantize_depth(view.world[2][1], view.depth_min, view.depth_max),
      0u);

  nx::vector<r2d::MeshVertex> vertices;
  nx::vector<u32> indices;
  const usize appended =
      walk(asset, view, [&](const Drawn &drawn, const ModelMesh &mesh) {
        const DrawableMesh source = drawable_mesh(asset, drawn.drawable);
        const usize d = nx::cast<usize>(drawn.drawable);
        const u32 first = mesh.first_vertex[d];

        vertices.clear();
        vertices.reserve(source.positions.size());
        for (usize v = 0; v < source.positions.size(); ++v) {
          const f32 x = source.positions[v].x;
          const f32 y = source.positions[v].y;
          r2d::MeshVertex vertex;
          vertex.position = glm::vec2(
              view.world[0][0] * x + view.world[1][0] * y + view.world[2][0],
              view.world[0][1] * x + view.world[1][1] * y + view.world[2][1]);
          vertex.uv = mesh.uvs[first + v];
          vertex.color = drawn.color;
          vertices.push_back(vertex);
        }
        indices.assign(mesh.indices.begin() + mesh.first_index[d],
                       mesh.indices.begin() + mesh.first_index[d + 1u]);

        r2d::MeshDraw &draw = out.append(vertices, indices);
        draw.texture = drawn.texture;
        draw.sort_key = key;
        draw.camera = view.camera;
        draw.blend = drawn.blend;
        draw.batch = view.batch;
        draw.material = view.material;
      });

  // Once per model rather than per drawable, and at all rather than never: a
  // face drawn without its clip is a visible defect and the log is where
  // someone looking at one starts.
  if (const usize masked = masked_drawable_count(asset); masked > 0)
    nx::logw("live2d: {} of {} drawables are clipped and this path does not "
             "mask; they will draw whole",
             masked, appended);
  return appended;
}

usize emit_draws(const ModelAsset &asset, const ModelView &view,
                 const MaskLayout *const masks, const u32 model,
                 nx::vector<ModelDraw> &out) {
  return walk(asset, view, [&](const Drawn &drawn, const ModelMesh &) {
    ModelDraw &draw = out.emplace_back();
    draw.model = model;
    draw.drawable = nx::cast<u32>(drawn.drawable);
    draw.texture = drawn.texture;
    draw.color = drawn.color;
    draw.blend = drawn.blend;
    if (masks == nullptr)
      return;
    const MaskRef &ref = masks->of(drawn.drawable);
    if (ref.clipped())
      draw.clip = {.group = ref.group,
                   .atlas = ref.atlas,
                   .channel = ref.channel,
                   .inverted = ref.inverted,
                   .to_mask = affine_of(ref.to_mask)};
  });
}

void copy_positions(const ModelAsset &asset,
                    const std::span<glm::vec2> out) noexcept {
  const csm::CubismModel *const model = asset.model();
  const ModelMesh *const mesh = asset.mesh().get();
  if (model == nullptr || mesh == nullptr || out.size() < mesh->vertex_count())
    return;
  auto *const m = const_cast<csm::CubismModel *>(model);
  const i32 count =
      nx::min(m->GetDrawableCount(), nx::cast<i32>(mesh->drawable_count()));
  for (i32 d = 0; d < count; ++d) {
    const usize du = nx::cast<usize>(d);
    const u32 first = mesh->first_vertex[du];
    const u32 vertices = mesh->first_vertex[du + 1u] - first;
    const core::csmVector2 *const points = m->GetDrawableVertexPositions(d);
    if (vertices == 0u || points == nullptr)
      continue;
    std::memcpy(out.data() + first, points, vertices * sizeof(glm::vec2));
  }
}

usize emit_mask_shapes(const ModelAsset &asset, const MaskLayout &masks,
                       const u32 model, nx::vector<MaskShape> &out) {
  const csm::CubismModel *const cubism = asset.model();
  const ModelMesh *const mesh = asset.mesh().get();
  if (cubism == nullptr || mesh == nullptr)
    return 0u;
  auto *const m = const_cast<csm::CubismModel *>(cubism);

  usize appended = 0;
  for (const MaskGroup &group : masks.groups()) {
    for (const i32 shape : group.shapes) {
      if (shape < 0 || nx::cast<u32>(shape) >= mesh->drawable_count())
        continue;
      const usize s = nx::cast<usize>(shape);
      if (mesh->first_index[s + 1u] == mesh->first_index[s])
        continue;
      out.push_back({
          .model = model,
          .drawable = nx::cast<u32>(shape),
          .texture = texture_of(asset, *m, shape),
          .atlas = group.atlas,
          .channel = group.channel,
          .to_mask = affine_of(group.to_mask),
          .tile = group.tile,
      });
      ++appended;
    }
  }
  return appended;
}
}
