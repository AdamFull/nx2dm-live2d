/**
 * @file live2d_module.cpp
 * @brief What tells an Engine about Live2D, and the only file here that knows
 * an Engine exists.
 */

#include "live2d/live2d_platform.h"

#include "core/app/engine.h"
#include "core/app/module.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::live2d {
namespace {

class Live2DModule final : public nxe::Module {
public:
  [[nodiscard]] nx::string_view name() const noexcept override {
    return "live2d";
  }

  bool on_register(nxe::Engine &) override {
    return install_platform();
  }

  void on_detach(nxe::Engine &) override { uninstall_platform(); }
};

} // namespace
} // namespace nxm::live2d

NX_DECLARE_MODULE(live2d, nxm::live2d::Live2DModule)
