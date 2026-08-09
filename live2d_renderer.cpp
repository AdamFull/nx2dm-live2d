/**
 * @file live2d_renderer.cpp
 * @brief The backend seam Cubism leaves open, and nothing behind it yet.
 */

#include "core/foundation/diagnostics/log.h"

#include <Rendering/CubismRenderer.hpp>

namespace Live2D::Cubism::Framework::Rendering {

CubismRenderer *CubismRenderer::Create(const csmUint32 width,
                                       const csmUint32 height) {
  (void)width;
  (void)height;
  nx::loge("live2d: no renderer backend is built; masks need one (L4)");
  return nullptr;
}

void CubismRenderer::StaticRelease() {}

} // namespace Live2D::Cubism::Framework::Rendering
