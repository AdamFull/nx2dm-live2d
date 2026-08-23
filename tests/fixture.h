#pragma once

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"

namespace nxm::live2d_test {

inline constexpr nx::string_view MODEL = "/Frieren/Frieren.model3.json";

[[nodiscard]] inline nx::string_view fixture_dir() noexcept {
  return NX_LIVE2D_FIXTURE_DIR;
}

[[nodiscard]] inline bool have_fixture() {
  const nx::string_view dir = fixture_dir();
  if (dir.empty())
    return false;
  return nx::fs::exists(nx::fs::path_view(nx::format("{}{}", dir, MODEL)));
}

}

#define NX_REQUIRE_FIXTURE()                                                   \
  do {                                                                         \
    if (!::nxm::live2d_test::have_fixture())                                   \
      SKIP("no model; set NX_LIVE2D_FIXTURE_DIR to a Cubism export");          \
  } while (false)
