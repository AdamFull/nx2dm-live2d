
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

constexpr nx::string_view ANGLE_X = "ParamAngleX";

constexpr f32 VISIBLE = 0.01f;

}

TEST_CASE("live2d: a deformed model lands inside the canvas the moc declares") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  if (!fixture.asset.motions().empty()) {
    const MotionEntry &first = fixture.asset.motions()[0];
    REQUIRE(fixture.animator.play(first.group.view(), first.index, true));
  }
  REQUIRE(fixture.animator.set_parameter(ANGLE_X, 25.f));
  for (u32 i = 0; i < 60; ++i)
    fixture.animator.update(1.f / 60.f);

  const CanvasInfo canvas = fixture.asset.canvas();
  const Bounds bounds = visible_bounds(fixture.asset);
  REQUIRE(bounds.valid());

  const f32 wide = canvas.width_units();
  const f32 tall = canvas.height_units();
  REQUIRE(wide > 0.f);
  REQUIRE(tall > 0.f);
  const f32 slack_x = wide * 0.1f;
  const f32 slack_y = tall * 0.1f;
  CHECK(bounds.min_x > -canvas.origin_x_units() - slack_x);
  CHECK(bounds.max_x < wide - canvas.origin_x_units() + slack_x);
  CHECK(bounds.min_y > -canvas.origin_y_units() - slack_y);
  CHECK(bounds.max_y < tall - canvas.origin_y_units() + slack_y);

  CHECK((bounds.max_x - bounds.min_x) > wide * 0.1f);
  CHECK((bounds.max_y - bounds.min_y) > tall * 0.1f);
}

TEST_CASE("live2d: the mesh and its texture agree about which way is up") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  fixture.animator.update(0.f);

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

  CHECK(largest_move(rest, turned) > VISIBLE);

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

  for (u32 i = 0; i < 10; ++i)
    fixture.animator.update(0.05f);
  const nx::vector<f32> later = snapshot(fixture.asset);
  nx::vector<f32> after;
  read_parameters(fixture.asset, after);

  CHECK(largest_move(before, after) > 0.5f);

  CHECK(largest_move(opening, later) > 0.f);

  CHECK(std::fabs(fixture.animator.elapsed() - 0.5f) < 1e-5f);
  CHECK_FALSE(fixture.animator.motion_finished());

  CHECK(visible_bounds(fixture.asset).valid());
}

TEST_CASE("live2d: physics keeps moving after the motion stops asking") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(fixture.asset.has_physics());

  for (u32 i = 0; i < 60; ++i)
    fixture.animator.update(1.f / 60.f);

  REQUIRE(fixture.animator.set_parameter(ANGLE_X, 30.f));
  fixture.animator.update(1.f / 60.f);
  const nx::vector<f32> released = snapshot(fixture.asset);

  fixture.animator.update(1.f / 60.f);
  const nx::vector<f32> settling = snapshot(fixture.asset);

  CHECK(largest_move(released, settling) > 0.f);
}

TEST_CASE("live2d: asking for a motion that is not there is refused") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  CHECK_FALSE(fixture.animator.play("no-such-group", 0));
  CHECK_FALSE(fixture.animator.set_expression("no-such-expression"));

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

  for (u32 i = 0; i < 30; ++i)
    fixture.animator.update(1.f / 60.f);
  CHECK(largest_move(plain, snapshot(fixture.asset)) > 0.f);
}
