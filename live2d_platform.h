#pragma once

/**
 * @file live2d_platform.h
 * @brief Bringing the Cubism SDK up on this engine's terms
 * (namespace nxm::live2d).
 */

#include "core/foundation/strings/utf8_string.h"

namespace nxm::live2d {

struct CoreVersion {
  u32 major = 0;
  u32 minor = 0;
  u32 patch = 0;

  [[nodiscard]] bool valid() const noexcept {
    return major != 0 || minor != 0 || patch != 0;
  }
};

[[nodiscard]] CoreVersion core_version() noexcept;

[[nodiscard]] u32 latest_moc_version() noexcept;

[[nodiscard]] bool install_platform();

void uninstall_platform();

} // namespace nxm::live2d
