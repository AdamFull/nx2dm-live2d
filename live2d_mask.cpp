#include "live2d/live2d_mask.h"

#include "core/foundation/diagnostics/log.h"

#include <Model/CubismModel.hpp>
#include <Rendering/CubismClippingManager.hpp>
#include <Rendering/CubismRenderer.hpp>

namespace nxm::live2d {
namespace {

namespace csm = Live2D::Cubism::Framework;
namespace render = Live2D::Cubism::Framework::Rendering;

struct NoTarget {};

class Context final : public render::CubismClippingContext {
public:
  Context(render::CubismClippingManager<Context, NoTarget> *,
          csm::CubismModel &, const csm::csmInt32 *indices,
          const csm::csmInt32 count)
      : CubismClippingContext(indices, count) {}
};

[[nodiscard]] glm::mat4 to_glm(const csm::CubismMatrix44 &m) noexcept {
  const f32 *const a = const_cast<csm::CubismMatrix44 &>(m).GetArray();
  glm::mat4 out(1.f);
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      out[column][row] = a[column * 4 + row];
  return out;
}

constexpr f32 MARGIN = 0.05f;

const MaskRef UNCLIPPED{};

}

class MaskLayout::Clipping final
    : public render::CubismClippingManager<Context, NoTarget> {
public:
  usize setup(csm::CubismModel &model) {
    usize using_count = 0;
    for (csm::csmUint32 i = 0; i < _clippingContextListForMask.GetSize(); ++i) {
      Context *const cc = _clippingContextListForMask[i];
      CalcClippedTotalBounds(
          model, cc, render::CubismRenderer::DrawableObjectType_Drawable);
      using_count += cc->_isUsing ? 1u : 0u;
    }
    if (using_count == 0)
      return 0u;

    SetupLayoutBounds(nx::cast<csm::csmInt32>(using_count));

    for (csm::csmUint32 i = 0; i < _clippingContextListForMask.GetSize(); ++i) {
      Context *const cc = _clippingContextListForMask[i];
      if (!cc->_isUsing)
        continue;

      csm::csmRectF *const clipped = cc->_allClippedDrawRect;
      csm::csmRectF *const tile = cc->_layoutBounds;
      _tmpBoundsOnModel.SetRect(clipped);
      _tmpBoundsOnModel.Expand(clipped->Width * MARGIN,
                               clipped->Height * MARGIN);

      // A degenerate rect divides to infinity and takes the whole layout with
      // it. It happens: a drawable scaled to nothing by an animation still has
      // bounds, and they are a point.
      if (_tmpBoundsOnModel.Width <= 0.f || _tmpBoundsOnModel.Height <= 0.f) {
        cc->_isUsing = false;
        --using_count;
        continue;
      }

      CreateMatrixForMask(false, tile, tile->Width / _tmpBoundsOnModel.Width,
                          tile->Height / _tmpBoundsOnModel.Height);
      cc->_matrixForMask.SetMatrix(_tmpMatrixForMask.GetArray());
      cc->_matrixForDraw.SetMatrix(_tmpMatrixForDraw.GetArray());
    }
    return using_count;
  }

  [[nodiscard]] csm::csmVector<Context *> &masks() noexcept {
    return _clippingContextListForMask;
  }
  [[nodiscard]] csm::csmVector<Context *> &per_drawable() noexcept {
    return _clippingContextListForDraw;
  }
};

MaskLayout::~MaskLayout() { CSM_DELETE(m_clipping); }

void MaskLayout::clear() noexcept {
  CSM_DELETE(m_clipping);
  m_clipping = nullptr;
  m_groups.clear();
  m_refs.clear();
  m_shapes.clear();
}

MaskLayout::MaskLayout(MaskLayout &&other) noexcept
    : m_clipping(other.m_clipping), m_groups(std::move(other.m_groups)),
      m_refs(std::move(other.m_refs)), m_shapes(std::move(other.m_shapes)),
      m_atlas_size(other.m_atlas_size), m_atlas_count(other.m_atlas_count) {
  other.m_clipping = nullptr;
}

MaskLayout &MaskLayout::operator=(MaskLayout &&other) noexcept {
  if (this != &other) {
    CSM_DELETE(m_clipping);
    m_clipping = other.m_clipping;
    m_groups = std::move(other.m_groups);
    m_refs = std::move(other.m_refs);
    m_shapes = std::move(other.m_shapes);
    m_atlas_size = other.m_atlas_size;
    m_atlas_count = other.m_atlas_count;
    other.m_clipping = nullptr;
  }
  return *this;
}

bool MaskLayout::build(const ModelAsset &asset, const u32 atlas_size,
                       const u32 atlas_count) {
  clear();

  csm::CubismModel *const model = asset.model();
  if (model == nullptr || atlas_size == 0 || atlas_count == 0)
    return false;

  m_atlas_size = atlas_size;
  m_atlas_count = atlas_count;

  m_clipping = CSM_NEW Clipping();
  m_clipping->SetClippingMaskBufferSize(nx::cast<f32>(atlas_size),
                                        nx::cast<f32>(atlas_size));
  m_clipping->Initialize(*model, nx::cast<csm::csmInt32>(atlas_count),
                         render::CubismRenderer::DrawableObjectType_Drawable);

  m_refs.resize(nx::cast<usize>(model->GetDrawableCount()));
  nx::logd("live2d: {} mask groups over {} drawables, {}x{} atlas x{}",
           m_clipping->masks().GetSize(), model->GetDrawableCount(), atlas_size,
           atlas_size, atlas_count);
  return true;
}

void MaskLayout::update(const ModelAsset &asset) {
  m_groups.clear();
  m_shapes.clear();
  for (MaskRef &ref : m_refs)
    ref = UNCLIPPED;

  csm::CubismModel *const model = asset.model();
  if (m_clipping == nullptr || model == nullptr)
    return;
  if (m_clipping->setup(*model) == 0u)
    return;

  csm::csmVector<Context *> &masks = m_clipping->masks();
  nx::vector<i32> group_of;
  group_of.resize(nx::cast<usize>(masks.GetSize()), -1);
  m_shapes.reserve(nx::cast<usize>(masks.GetSize()));

  for (csm::csmUint32 i = 0; i < masks.GetSize(); ++i) {
    Context *const cc = masks[i];
    if (!cc->_isUsing)
      continue;

    nx::vector<i32> shapes;
    shapes.reserve(nx::cast<usize>(cc->_clippingIdCount));
    for (csm::csmInt32 s = 0; s < cc->_clippingIdCount; ++s)
      shapes.push_back(cc->_clippingIdList[s]);

    const csm::csmRectF *const tile = cc->_layoutBounds;
    group_of[i] = nx::cast<i32>(m_groups.size());
    m_shapes.push_back(std::move(shapes));
    m_groups.push_back({
        .atlas = nx::cast<u32>(cc->_bufferIndex),
        .channel = nx::cast<u32>(cc->_layoutChannelIndex),
        .to_mask = to_glm(cc->_matrixForMask),
        .tile = {tile->X * 2.f - 1.f, tile->Y * 2.f - 1.f,
                 (tile->X + tile->Width) * 2.f - 1.f,
                 (tile->Y + tile->Height) * 2.f - 1.f},
        .shapes = {},
    });
  }
  // Spans are filled after the vector has stopped growing; taking them as the
  // groups were pushed would leave every one but the last dangling.
  for (usize i = 0; i < m_groups.size(); ++i)
    m_groups[i].shapes = {m_shapes[i].data(), m_shapes[i].size()};

  csm::csmVector<Context *> &per_drawable = m_clipping->per_drawable();
  const usize drawables =
      nx::min(m_refs.size(), nx::cast<usize>(per_drawable.GetSize()));
  for (usize d = 0; d < drawables; ++d) {
    const Context *const cc = per_drawable[nx::cast<csm::csmUint32>(d)];
    if (cc == nullptr || !cc->_isUsing)
      continue;

    i32 group = -1;
    for (csm::csmUint32 i = 0; i < masks.GetSize(); ++i)
      if (masks[i] == cc) {
        group = group_of[i];
        break;
      }
    if (group < 0)
      continue;

    m_refs[d] = {
        .group = group,
        .atlas = nx::cast<u32>(cc->_bufferIndex),
        .channel = nx::cast<u32>(cc->_layoutChannelIndex),
        .inverted =
            model->GetDrawableInvertedMask(nx::cast<csm::csmInt32>(d)) != 0,
        .to_mask = to_glm(cc->_matrixForDraw),
    };
  }
}

const MaskRef &MaskLayout::of(const i32 drawable) const noexcept {
  if (drawable < 0 || nx::cast<usize>(drawable) >= m_refs.size())
    return UNCLIPPED;
  return m_refs[nx::cast<usize>(drawable)];
}

}
