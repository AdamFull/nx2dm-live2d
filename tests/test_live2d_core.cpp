/**
 * @file test_live2d_core.cpp
 * @brief That the Cubism SDK is here, is the one we think it is, and runs.
 *
 * Trivial by weight and not by worth. Cubism Core is a binary chosen at
 * configure time out of a matrix of platform, architecture, MSVC toolset and
 * CRT, and picking the wrong cell is a linker error on a machine nobody built
 * on this week. This is the case that turns that into a red test.
 */

#include "framework/nxtest.h"

#include "live2d/live2d_platform.h"

namespace {

using namespace nxm::live2d;

/// The moc3 format numbers Core reports, from Live2DCubismCore.h. Spelled here
/// rather than included: the enum lives inside Live2D::Cubism::Core and this
/// file has no business opening that namespace to name one integer.
constexpr u32 MOC_VERSION_42 = 4;
constexpr u32 MOC_VERSION_53 = 6;

} // namespace

TEST_CASE("live2d: a Core binary linked, and it answers") {
  const CoreVersion version = core_version();

  // The whole claim. A version at all means the right cell of the platform x
  // architecture x toolset x CRT matrix was picked and the binary loaded;
  // anything more specific is a pin on a number that is not ours.
  //
  // In particular do not check it against the SDK's: "CubismSdkForNative
  // 5-r.5" ships Core 06.00.0001, and its own changelog says so. The two are
  // versioned separately and the branding is the one that lags.
  CHECK(version.valid());
  CHECK(version.major >= 5u);
}

TEST_CASE("live2d: Core reads every moc3 format the fixture could be in") {
  const u32 latest = latest_moc_version();

  // The Frieren fixture is 4.2. Data from a newer editor than the runtime does
  // not load at all, so this is the check that turns "the model is blank" into
  // a named cause.
  CHECK(latest >= MOC_VERSION_42);
  CHECK(latest == MOC_VERSION_53);
}

TEST_CASE("live2d: the framework starts on nx's allocator, twice if asked") {
  REQUIRE(install_platform());

  // Idempotent because a module's on_register and a headless test may both
  // reach it, and Cubism's own StartUp quietly does nothing the second time
  // rather than reporting it.
  CHECK(install_platform());

  uninstall_platform();
  // And comes back: CleanUp is what makes a second StartUp mean anything, so a
  // test binary running case after case is not one-shot.
  CHECK(install_platform());
  uninstall_platform();
}
