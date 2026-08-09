#include "live2d/live2d_animator.h"

#include "core/foundation/diagnostics/log.h"

#include <Effect/CubismBreath.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismUserModel.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Physics/CubismPhysics.hpp>

#include <cmath>

namespace nxm::live2d {
namespace {

namespace csm = Live2D::Cubism::Framework;

class Exposed final : public csm::CubismUserModel {
public:
  using csm::CubismUserModel::_breath;
  using csm::CubismUserModel::_expressionManager;
  using csm::CubismUserModel::_eyeBlink;
  using csm::CubismUserModel::_model;
  using csm::CubismUserModel::_motionManager;
  using csm::CubismUserModel::_physics;
  using csm::CubismUserModel::_pose;
};

[[nodiscard]] Exposed *reach(const ModelAsset *const asset) noexcept {
  if (asset == nullptr || !asset->valid())
    return nullptr;
  return static_cast<Exposed *>(asset->owner());
}

constexpr i32 MOTION_PRIORITY = 2;

} // namespace

bool Animator::play(const nx::string_view group, const i32 index,
                    const bool loop) {
  Exposed *const owner = reach(m_asset);
  if (owner == nullptr)
    return false;

  csm::ACubismMotion *const motion = m_asset->find_motion(group, index);
  if (motion == nullptr) {
    nx::logw("live2d: no motion '{}' at {}", group, index);
    return false;
  }
  motion->SetLoop(loop);
  owner->_motionManager->StartMotionPriority(motion, false, MOTION_PRIORITY);
  return true;
}

bool Animator::set_expression(const nx::string_view name) {
  Exposed *const owner = reach(m_asset);
  if (owner == nullptr)
    return false;

  csm::ACubismMotion *const motion = m_asset->find_expression(name);
  if (motion == nullptr) {
    nx::logw("live2d: no expression '{}'", name);
    return false;
  }
  owner->_expressionManager->StartMotion(motion, false);
  m_expression = name;
  return true;
}

void Animator::update(const f32 dt) {
  Exposed *const owner = reach(m_asset);
  if (owner == nullptr)
    return;
  csm::CubismModel *const model = owner->_model;
  m_elapsed += dt;

  model->LoadParameters();
  owner->_motionManager->UpdateMotion(model, dt);
  model->SaveParameters();

  if (owner->_expressionManager != nullptr)
    owner->_expressionManager->UpdateMotion(model, dt);

  // Cubism's order, and each of these writes parameters the next reads.
  if (m_blinking && owner->_eyeBlink != nullptr)
    owner->_eyeBlink->UpdateParameters(model, dt);
  if (m_breathing && owner->_breath != nullptr)
    owner->_breath->UpdateParameters(model, dt);

  if (owner->_physics != nullptr)
    owner->_physics->Evaluate(model, dt);

  // After physics, as LAppModel does: a mouth is not something inertia should
  // lag, and a voice that has stopped should close it now rather than settle.
  for (const nx::string &id : m_asset->lip_sync())
    model->SetParameterValue(
        csm::CubismFramework::GetIdManager()->GetId(id.c_str()), m_mouth, 0.8f);

  if (owner->_pose != nullptr)
    owner->_pose->UpdateParameters(model, dt);

  model->Update();
}

bool Animator::set_parameter(const nx::string_view id, const f32 value) {
  Exposed *const owner = reach(m_asset);
  if (owner == nullptr)
    return false;
  const nx::string owned(id);
  owner->_model->SetParameterValue(
      csm::CubismFramework::GetIdManager()->GetId(owned.c_str()), value);
  return true;
}

f32 Animator::parameter(const nx::string_view id) const {
  const Exposed *const owner = reach(m_asset);
  if (owner == nullptr)
    return 0.f;
  const nx::string owned(id);
  return const_cast<Exposed *>(owner)->_model->GetParameterValue(
      csm::CubismFramework::GetIdManager()->GetId(owned.c_str()));
}

void Animator::refresh() {
  Exposed *const owner = reach(m_asset);
  if (owner != nullptr)
    owner->_model->Update();
}

bool Animator::motion_finished() const noexcept {
  const Exposed *const owner = reach(m_asset);
  if (owner == nullptr)
    return true;
  return const_cast<Exposed *>(owner)->_motionManager->IsFinished();
}

void read_vertices(const ModelAsset &asset, nx::vector<f32> &out) {
  out.clear();
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return;
  auto *const m = const_cast<csm::CubismModel *>(model);

  for (i32 d = 0; d < m->GetDrawableCount(); ++d) {
    if (!m->GetDrawableDynamicFlagIsVisible(d))
      continue;
    const i32 count = m->GetDrawableVertexCount(d);
    const f32 *const xy = m->GetDrawableVertices(d);
    if (count <= 0 || xy == nullptr)
      continue;
    out.insert(out.end(), xy, xy + nx::cast<usize>(count) * 2u);
  }
}

void read_parameters(const ModelAsset &asset, nx::vector<f32> &out) {
  out.clear();
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return;
  auto *const m = const_cast<csm::CubismModel *>(model);
  out.reserve(nx::cast<usize>(m->GetParameterCount()));
  for (i32 i = 0; i < m->GetParameterCount(); ++i)
    out.push_back(m->GetParameterValue(i));
}

f32 uv_agreement(const ModelAsset &asset) noexcept {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return 0.f;
  auto *const m = const_cast<csm::CubismModel *>(model);

  f64 weighted = 0.0;
  f64 total = 0.0;
  for (i32 d = 0; d < m->GetDrawableCount(); ++d) {
    const i32 count = m->GetDrawableVertexCount(d);
    const Live2D::Cubism::Core::csmVector2 *const points =
        m->GetDrawableVertexPositions(d);
    const Live2D::Cubism::Core::csmVector2 *const uvs =
        m->GetDrawableVertexUvs(d);
    if (count < 3 || points == nullptr || uvs == nullptr)
      continue;

    f64 mean_y = 0.0;
    f64 mean_v = 0.0;
    for (i32 i = 0; i < count; ++i) {
      mean_y += points[i].Y;
      mean_v += uvs[i].Y;
    }
    mean_y /= count;
    mean_v /= count;

    f64 covariance = 0.0;
    f64 var_y = 0.0;
    f64 var_v = 0.0;
    for (i32 i = 0; i < count; ++i) {
      const f64 dy = points[i].Y - mean_y;
      const f64 dv = uvs[i].Y - mean_v;
      covariance += dy * dv;
      var_y += dy * dy;
      var_v += dv * dv;
    }
    // A drawable with no spread either way says nothing about orientation.
    if (var_y <= 0.0 || var_v <= 0.0)
      continue;

    weighted += (covariance / std::sqrt(var_y * var_v)) * count;
    total += count;
  }
  return total > 0.0 ? nx::cast<f32>(weighted / total) : 0.f;
}

Bounds visible_bounds(const ModelAsset &asset) noexcept {
  const csm::CubismModel *const model = asset.model();
  if (model == nullptr)
    return {};
  auto *const m = const_cast<csm::CubismModel *>(model);

  bool any = false;
  Bounds bounds;
  for (i32 d = 0; d < m->GetDrawableCount(); ++d) {
    if (!m->GetDrawableDynamicFlagIsVisible(d))
      continue;
    const i32 count = m->GetDrawableVertexCount(d);
    const Live2D::Cubism::Core::csmVector2 *const points =
        m->GetDrawableVertexPositions(d);
    if (count <= 0 || points == nullptr)
      continue;

    for (i32 v = 0; v < count; ++v) {
      const f32 x = points[v].X;
      const f32 y = points[v].Y;
      if (!any) {
        bounds = {x, y, x, y};
        any = true;
        continue;
      }
      bounds.min_x = nx::min(bounds.min_x, x);
      bounds.min_y = nx::min(bounds.min_y, y);
      bounds.max_x = nx::max(bounds.max_x, x);
      bounds.max_y = nx::max(bounds.max_y, y);
    }
  }
  return bounds;
}

} // namespace nxm::live2d
