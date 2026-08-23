
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_draw.h"
#include "live2d/live2d_mask.h"

#include <cmath>

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;

struct Loaded {
  bool ok = false;
  ModelAsset asset;
  Animator animator;
  MaskLayout masks;

  Loaded() {
    nx::vfs::initialize();
    nx::vfs::Device *const host =
        nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
    if (host == nullptr || !nx::vfs::mount("/", host))
      return;
    nx::string error;
    ok = load_model(MODEL, {}, asset, error) && masks.build(asset);
    animator.bind(&asset);
    animator.update(0.f);
    masks.update(asset);
  }
  ~Loaded() {
    asset = ModelAsset();
    nx::vfs::shutdown();
  }

  Loaded(const Loaded &) = delete;
  Loaded &operator=(const Loaded &) = delete;
};

[[nodiscard]] glm::vec2 through(const glm::mat4 &m, const f32 x,
                                const f32 y) noexcept {
  const glm::vec4 p = m * glm::vec4(x, y, 0.f, 1.f);
  return {p.x, p.y};
}

}

TEST_CASE("live2d: every clipped drawable is given a mask, and no other is") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(fixture.masks.active());

  const usize expected = masked_drawable_count(fixture.asset);
  REQUIRE(expected > 0u);

  usize clipped = 0;
  usize clipped_and_visible = 0;
  for (i32 d = 0; d < nx::cast<i32>(fixture.asset.drawable_count()); ++d) {
    if (!fixture.masks.of(d).clipped())
      continue;
    ++clipped;
    clipped_and_visible += drawable_visible(fixture.asset, d) ? 1u : 0u;
  }
  CHECK(clipped_and_visible == expected);
  CHECK(clipped >= expected);

  CHECK_FALSE(fixture.masks.of(-1).clipped());
  CHECK_FALSE(
      fixture.masks.of(nx::cast<i32>(fixture.asset.drawable_count()) + 10)
          .clipped());
}

TEST_CASE("live2d: masks pack into the atlas's channels without colliding") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  const std::span<const MaskGroup> groups = fixture.masks.groups();
  REQUIRE(!groups.empty());

  for (const MaskGroup &group : groups) {
    CHECK(group.channel < 4u);
    CHECK(group.atlas < fixture.masks.atlas_count());
    CHECK(!group.shapes.empty());
    for (const i32 shape : group.shapes) {
      CHECK(shape >= 0);
      CHECK(shape < nx::cast<i32>(fixture.asset.drawable_count()));
    }
  }

  for (i32 d = 0; d < nx::cast<i32>(fixture.asset.drawable_count()); ++d) {
    const MaskRef &ref = fixture.masks.of(d);
    if (!ref.clipped())
      continue;
    REQUIRE(nx::cast<usize>(ref.group) < groups.size());
    const MaskGroup &group = groups[nx::cast<usize>(ref.group)];
    CHECK(ref.atlas == group.atlas);
    CHECK(ref.channel == group.channel);
  }
}

TEST_CASE("live2d: a mask's shapes reach the tile they are drawn into") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  const std::span<const MaskGroup> groups = fixture.masks.groups();
  REQUIRE(!groups.empty());

  for (const MaskGroup &group : groups) {
    usize inside = 0;
    usize total = 0;
    for (const i32 shape : group.shapes) {
      const DrawableMesh mesh = drawable_mesh(fixture.asset, shape);
      if (!mesh.valid())
        continue;
      for (const glm::vec2 &point : mesh.positions) {
        const glm::vec2 p = through(group.to_mask, point.x, point.y);
        inside +=
            p.x >= -1.f && p.x <= 1.f && p.y >= -1.f && p.y <= 1.f ? 1u : 0u;
        ++total;
      }
    }
    REQUIRE(total > 0u);
    CHECK(inside > 0u);
  }
}

TEST_CASE("live2d: a clipped drawable samples the tile its mask was drawn "
          "into") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  usize checked = 0;
  for (i32 d = 0; d < nx::cast<i32>(fixture.asset.drawable_count()); ++d) {
    const MaskRef &ref = fixture.masks.of(d);
    if (!ref.clipped() || !drawable_visible(fixture.asset, d))
      continue;

    const DrawableMesh mesh = drawable_mesh(fixture.asset, d);
    if (!mesh.valid())
      continue;

    for (const glm::vec2 &point : mesh.positions) {
      const glm::vec2 uv = through(ref.to_mask, point.x, point.y);
      CHECK(uv.x >= 0.f);
      CHECK(uv.x <= 1.f);
      CHECK(uv.y >= 0.f);
      CHECK(uv.y <= 1.f);
    }
    ++checked;
  }
  CHECK(checked > 0u);
}

TEST_CASE("live2d: the layout follows the pose rather than the bind") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(!fixture.masks.groups().empty());

  const glm::mat4 before = fixture.masks.groups()[0].to_mask;

  REQUIRE(fixture.animator.set_parameter("ParamAngleX", 30.f));
  fixture.animator.refresh();
  fixture.masks.update(fixture.asset);
  REQUIRE(!fixture.masks.groups().empty());
  const glm::mat4 after = fixture.masks.groups()[0].to_mask;

  f32 moved = 0.f;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      moved = nx::max(moved, std::fabs(after[c][r] - before[c][r]));
  CHECK(moved > 0.f);
}

TEST_CASE("live2d: a model with no masks lays out nothing") {
  MaskLayout empty;
  ModelAsset none;
  CHECK_FALSE(empty.build(none));
  CHECK_FALSE(empty.active());
  CHECK(empty.groups().empty());
  CHECK_FALSE(empty.of(0).clipped());

  empty.update(none);
  CHECK_FALSE(empty.active());
}
