/**
 * @file test_live2d_mask.cpp
 * @brief Where each clipping mask lands in the atlas.
 *
 * No device anywhere in this file. Packing masks four to a texture and working
 * out the two matrices each one needs is arithmetic, and arithmetic that is
 * subtly wrong gives a model that is *almost* right - which is exactly the sort
 * of thing that survives being looked at and does not survive being measured.
 */

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

/// Where a model-space point ends up under one of the layout's matrices.
[[nodiscard]] glm::vec2 through(const glm::mat4 &m, const f32 x,
                                const f32 y) noexcept {
  const glm::vec4 p = m * glm::vec4(x, y, 0.f, 1.f);
  return {p.x, p.y};
}

} // namespace

TEST_CASE("live2d: every clipped drawable is given a mask, and no other is") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(fixture.masks.active());

  // The emitter counted these independently, off the model's own mask counts.
  // Both numbers coming from different code and agreeing is the claim - but
  // only like for like: masked_drawable_count asks about drawables that will
  // be drawn, and the layout registers every clipped drawable whether or not
  // the pose currently shows it. Restricted to the visible ones they match.
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

  // Out of range asks are answered, not indexed.
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
    // A group with no shapes would render an empty mask and clip everything
    // that uses it to nothing.
    CHECK(!group.shapes.empty());
    for (const i32 shape : group.shapes) {
      CHECK(shape >= 0);
      CHECK(shape < nx::cast<i32>(fixture.asset.drawable_count()));
    }
  }

  // Every drawable's ref names a real group, and agrees with it about which
  // channel to sample - the two are written from the same context and a
  // mismatch would sample a neighbour's mask.
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

  // Overlap, not containment. The tile is fitted to the bounds of the art
  // being clipped, so a mask shape larger than what it clips legitimately
  // spills past the edge and the viewport takes the excess. What would be
  // broken is a mask that misses its tile altogether: every drawable using it
  // would clip to nothing and vanish.
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

  // to_mask for a drawable is the other half of the pair: it turns model space
  // into the atlas UV where the mask went. If it disagreed with the group's own
  // matrix, the drawable would sample the wrong part of the texture - which
  // looks like a mask that is offset or scaled, not like a missing one.
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
      // Tight, and it can be: this is the art the tile was fitted to, with a
      // five percent margin around it, so it lands inside the atlas's 0..1
      // with room to spare. Anything looser would pass with the drawable
      // sampling a neighbouring mask.
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

  // A mask's tile is fitted to the bounds of what it clips, so moving that art
  // has to move the fit. A layout computed once at load would not.
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

  // And update on an unbuilt layout is a no-op rather than a crash.
  empty.update(none);
  CHECK_FALSE(empty.active());
}
