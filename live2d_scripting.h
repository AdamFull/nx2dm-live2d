#pragma once

/**
 * @file live2d_scripting.h
 * @brief What a script may do to a model (namespace nxm::live2d).
 */

#include "live2d/live2d_system.h"

namespace nxe {
class Engine;
namespace script {
class Host;
}
} // namespace nxe

namespace nxm::live2d {

void expose_live2d_services(nxe::script::Host &host, nxe::Engine &engine);

usize drive_lip_sync(nxe::Engine &engine, Live2DSystem &system);

} // namespace nxm::live2d
