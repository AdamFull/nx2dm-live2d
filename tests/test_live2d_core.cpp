
#include "framework/nxtest.h"

#include "live2d/live2d_platform.h"

namespace {

using namespace nxm::live2d;

constexpr u32 MOC_VERSION_42 = 4;
constexpr u32 MOC_VERSION_53 = 6;

}

TEST_CASE("live2d: a Core binary linked, and it answers") {
  const CoreVersion version = core_version();

  CHECK(version.valid());
  CHECK(version.major >= 5u);
}

TEST_CASE("live2d: Core reads every moc3 format the fixture could be in") {
  const u32 latest = latest_moc_version();

  CHECK(latest >= MOC_VERSION_42);
  CHECK(latest == MOC_VERSION_53);
}

TEST_CASE("live2d: the framework starts on nx's allocator, twice if asked") {
  REQUIRE(install_platform());

  CHECK(install_platform());

  uninstall_platform();
  CHECK(install_platform());
  uninstall_platform();
}
