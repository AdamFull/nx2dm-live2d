#pragma once

/**
 * @file fixture.h
 * @brief Where the model these cases are written against lives, and what to do
 * when it does not (namespace nxm::live2d_test).
 *
 * The fixture is not in this repository and will not be: the model used to
 * develop this is a VTube Studio export of a copyrighted character, which is
 * not ours to redistribute. Point NX_LIVE2D_FIXTURE_DIR at a directory holding
 * one and these cases run; without one they skip.
 *
 * Any Cubism 3+ export works for the cases that only assert structure. The
 * numbers below are the development model's, so a different one skips those.
 */

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

} // namespace nxm::live2d_test

#define NX_REQUIRE_FIXTURE()                                                   \
  do {                                                                         \
    if (!::nxm::live2d_test::have_fixture())                                   \
      SKIP("no model; set NX_LIVE2D_FIXTURE_DIR to a Cubism export");          \
  } while (false)
