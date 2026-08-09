/**
 * @file test_live2d_animate.cpp
 * @brief Parameters into vertices: motions, physics, and where the result is.
 *
 * One lesson from the Spine suite runs through this file. Every case there
 * compared one pose against another, so all of them passed for a phase with the
 * skeleton rendered upside down - a comparison cannot catch an error that moves
 * both sides equally. So the deformation is checked against the canvas the moc
 * itself declares, which is a fact about the model rather than about the last
 * frame.
 */

#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_animator.h"

#include <cmath>

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;

struct Loaded {
  bool ok = false;
  ModelAsset asset;
  Animator animator;

  Loaded() {
    nx::vfs::initialize();
    nx::vfs::Device *const host =
        nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
    if (host == nullptr || !nx::vfs::mount("/", host))
      return;
    nx::string error;
    ok = load_model(MODEL, {}, asset, error);
    animator.bind(&asset);
  }
  ~Loaded() {
    asset = ModelAsset();
    nx::vfs::shutdown();
  }

  Loaded(const Loaded &) = delete;
  Loaded &operator=(const Loaded &) = delete;
};

[[nodiscard]] nx::vector<f32> snapshot(const ModelAsset &asset) {
  nx::vector<f32> out;
  read_vertices(asset, out);
  return out;
}

[[nodiscard]] f32 largest_move(const nx::vector<f32> &a,
                               const nx::vector<f32> &b) {
  f32 worst = 0.f;
  const usize n = nx::min(a.size(), b.size());
  for (usize i = 0; i < n; ++i)
    worst = nx::max(worst, std::fabs(a[i] - b[i]));
  return worst;
}

/// The one parameter every Cubism model is guaranteed to have, and the one a
/// case can move without knowing anything about the rig.
constexpr nx::string_view ANGLE_X = "ParamAngleX";

/// A move worth calling a move, in model units. The development model is about
/// one unit across, so a hundredth of that is a visible shift of the head and
/// far above anything float noise produces.
constexpr f32 VISIBLE = 0.01f;

} // namespace

TEST_CASE("live2d: a deformed model lands inside the canvas the moc declares") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  // Deformed rather than at rest, so the name is true: a motion running and a
  // parameter driven by hand, then a second of frames for physics to settle
  // into. A bind pose would pass this without the deformers ever running.
  if (!fixture.asset.motions().empty()) {
    const MotionEntry &first = fixture.asset.motions()[0];
    REQUIRE(fixture.animator.play(first.group.view(), first.index, true));
  }
  REQUIRE(fixture.animator.set_parameter(ANGLE_X, 25.f));
  for (u32 i = 0; i < 60; ++i)
    fixture.animator.update(1.f / 60.f);

  // The absolute claim, and the reason this file exists. A model rendered
  // upside down, mirrored, or at a thousand times its size passes every
  // pose-against-pose comparison in this suite and fails here.
  const CanvasInfo canvas = fixture.asset.canvas();
  const Bounds bounds = visible_bounds(fixture.asset);
  REQUIRE(bounds.valid());

  // Units, not pixels. The canvas is 5167 x 9410 pixels and about 1 x 2 units,
  // and the vertices are in the second of those - which is worth spelling out
  // because comparing against the first passes nothing and looks like a broken
  // rig rather than a broken test.
  const f32 wide = canvas.width_units();
  const f32 tall = canvas.height_units();
  REQUIRE(wide > 0.f);
  REQUIRE(tall > 0.f);
  // The canvas in the vertices' own space: the moc puts its origin inside the
  // canvas, so the box runs from -origin to size-origin. Frieren's is
  // x [-0.5, 0.5], y [-0.91, 0.91], and the model sits inside it with room to
  // spare. Art may overhang a little, hence the tenth.
  const f32 slack_x = wide * 0.1f;
  const f32 slack_y = tall * 0.1f;
  CHECK(bounds.min_x > -canvas.origin_x_units() - slack_x);
  CHECK(bounds.max_x < wide - canvas.origin_x_units() + slack_x);
  CHECK(bounds.min_y > -canvas.origin_y_units() - slack_y);
  CHECK(bounds.max_y < tall - canvas.origin_y_units() + slack_y);

  // And it occupies a real share of it rather than collapsing to a point.
  CHECK((bounds.max_x - bounds.min_x) > wide * 0.1f);
  CHECK((bounds.max_y - bounds.min_y) > tall * 0.1f);
}

TEST_CASE("live2d: the mesh and its texture agree about which way is up") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  fixture.animator.update(0.f);

  // The case the bounds check above cannot make. Frieren's art is roughly
  // centred on the origin, so flipping every vertex's Y leaves the model
  // inside the same box and every frame-to-frame comparison unchanged - which
  // is precisely how a phase of Spine tests passed with the skeleton upside
  // down. Vertex Y against texture V is the one thing a flip cannot fake.
  // Half rather than the 0.9-plus this model actually measures: a model with
  // art rotated in its atlas legitimately scores lower, and the failure being
  // guarded against is around -0.9.
  CHECK(uv_agreement(fixture.asset) > 0.5f);
}

TEST_CASE("live2d: a parameter moves vertices, and the same one twice does "
          "not") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  fixture.animator.update(0.f);
  const nx::vector<f32> rest = snapshot(fixture.asset);
  REQUIRE(!rest.empty());

  REQUIRE(fixture.animator.set_parameter(ANGLE_X, 30.f));
  CHECK(fixture.animator.parameter(ANGLE_X) == 30.f);
  fixture.animator.refresh();
  const nx::vector<f32> turned = snapshot(fixture.asset);

  // Something actually moved, and by an amount you could see.
  CHECK(largest_move(rest, turned) > VISIBLE);

  // Deterministic: the same parameters give the same vertices, so a later
  // case comparing two frames is comparing the animation and not noise.
  REQUIRE(fixture.animator.set_parameter(ANGLE_X, 30.f));
  fixture.animator.refresh();
  CHECK(largest_move(turned, snapshot(fixture.asset)) == 0.f);
}

TEST_CASE("live2d: a motion drives parameters, and they reach the vertices") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(!fixture.asset.motions().empty());

  const MotionEntry &first = fixture.asset.motions()[0];
  REQUIRE(fixture.animator.play(first.group.view(), first.index, true));

  fixture.animator.update(0.f);
  const nx::vector<f32> opening = snapshot(fixture.asset);
  nx::vector<f32> before;
  read_parameters(fixture.asset, before);
  REQUIRE(!before.empty());

  // Half a second in ten steps rather than one leap: physics integrates, so a
  // single huge dt is not the same animation and would hide a stage that only
  // works at small steps.
  for (u32 i = 0; i < 10; ++i)
    fixture.animator.update(0.05f);
  const nx::vector<f32> later = snapshot(fixture.asset);
  nx::vector<f32> after;
  read_parameters(fixture.asset, after);

  // Parameters, not vertices, are where "the motion is playing" is a sharp
  // claim. This model's motions key four custom parameters that move small
  // details, so its whole-mesh displacement over half a second is about
  // 0.0004 units - real, and far too small to be a threshold worth trusting.
  // Which parameters is not hardcoded: any of them moving is the claim.
  CHECK(largest_move(before, after) > 0.5f);

  // And what the motion asked for reached the deformers rather than stopping
  // at the parameter table.
  CHECK(largest_move(opening, later) > 0.f);

  // Ten additions of 0.05f, so not exactly a half.
  CHECK(std::fabs(fixture.animator.elapsed() - 0.5f) < 1e-5f);
  // Looping, so it has not run out.
  CHECK_FALSE(fixture.animator.motion_finished());

  // And it is still a model, not an explosion of vertices.
  CHECK(visible_bounds(fixture.asset).valid());
}

TEST_CASE("live2d: physics keeps moving after the motion stops asking") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(fixture.asset.has_physics());

  // Settle, then yank the head round and let go. Physics is the only stage
  // that can still be moving vertices on a frame where no parameter changed.
  for (u32 i = 0; i < 60; ++i)
    fixture.animator.update(1.f / 60.f);

  REQUIRE(fixture.animator.set_parameter(ANGLE_X, 30.f));
  fixture.animator.update(1.f / 60.f);
  const nx::vector<f32> released = snapshot(fixture.asset);

  fixture.animator.update(1.f / 60.f);
  const nx::vector<f32> settling = snapshot(fixture.asset);

  // No parameter was set on that second step, so anything that moved was the
  // rig's own inertia.
  CHECK(largest_move(released, settling) > 0.f);
}

TEST_CASE("live2d: asking for a motion that is not there is refused") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  CHECK_FALSE(fixture.animator.play("no-such-group", 0));
  CHECK_FALSE(fixture.animator.set_expression("no-such-expression"));

  // And an unbound animator does nothing rather than dereferencing nothing.
  Animator loose;
  CHECK_FALSE(loose.play("anything", 0));
  loose.update(0.016f);
  CHECK(loose.motion_finished());
}

TEST_CASE("live2d: an expression is remembered by the name it was set with") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(!fixture.asset.expressions().empty());

  const nx::string name(fixture.asset.expressions()[0].name);
  fixture.animator.update(0.f);
  const nx::vector<f32> plain = snapshot(fixture.asset);

  REQUIRE(fixture.animator.set_expression(name.view()));
  CHECK(fixture.animator.expression() == name.view());

  // Expressions fade in, so one step is not enough to see them.
  for (u32 i = 0; i < 30; ++i)
    fixture.animator.update(1.f / 60.f);
  CHECK(largest_move(plain, snapshot(fixture.asset)) > 0.f);
}
