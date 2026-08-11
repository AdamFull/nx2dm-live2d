#pragma once

/**
 * @file live2d_scripting.h
 * @brief What a script may do to a model (namespace nxm::live2d).
 */

#include "live2d/live2d_system.h"

namespace nxe {
class ModuleContext;
namespace script {
class Host;
}
} // namespace nxe

namespace nxm::live2d {

void expose_live2d_services(nxe::script::Host &host, nxe::ModuleContext &ctx);

usize drive_lip_sync(nxe::ModuleContext &ctx, Live2DSystem &system);

} // namespace nxm::live2d
