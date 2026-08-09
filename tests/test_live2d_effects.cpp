/**
 * @file test_live2d_effects.cpp
 * @brief Blink, breath and lip sync - the three things a model does when
 * nothing is animating it.
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

/// The largest and smallest a parameter gets over @p steps of a sixtieth.
struct Swing {
  f32 low = 0.f;
  f32 high = 0.f;
  [[nodiscard]] f32 range() const noexcept { return high - low; }
};

[[nodiscard]] Swing sweep(Animator &animator, const nx::string_view id,
                          const u32 steps) {
  Swing out{1e9f, -1e9f};
  for (u32 i = 0; i < steps; ++i) {
    animator.update(1.f / 60.f);
    const f32 value = animator.parameter(id);
    out.low = nx::min(out.low, value);
    out.high = nx::max(out.high, value);
  }
  return out;
}

} // namespace

TEST_CASE("live2d: the manifest's own parameter groups are picked up") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  // The development model groups two eyes and one mouth. Both groups are
  // optional in the format, so this is a fact about the fixture rather than
  // about Cubism - which is why nothing here fails without them.
  CHECK(fixture.asset.has_eye_blink());
  REQUIRE(fixture.asset.lip_sync().size() == 1u);
  CHECK(fixture.asset.lip_sync()[0] == "ParamMouthOpenY");
}

TEST_CASE("live2d: blinking closes the eyes, and only when asked") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(fixture.asset.has_eye_blink());

  // Off by default: ten seconds is several blink intervals, and a model that
  // was told to blink is the only one that should.
  const Swing still = sweep(fixture.animator, "ParamEyeLOpen", 600);
  CHECK(still.range() == 0.f);

  fixture.animator.set_blinking(true);
  CHECK(fixture.animator.blinking());
  const Swing blinking = sweep(fixture.animator, "ParamEyeLOpen", 600);

  // An eye parameter runs 0 (shut) to 1 (open), so a blink is most of that.
  CHECK(blinking.range() > 0.5f);
  CHECK(blinking.low < 0.2f);
  CHECK(blinking.high > 0.8f);
}

TEST_CASE("live2d: breathing sways the model, and only when asked") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  const Swing still = sweep(fixture.animator, "ParamBreath", 400);
  CHECK(still.range() == 0.f);

  fixture.animator.set_breathing(true);
  CHECK(fixture.animator.breathing());
  // A cycle is about 3.2 seconds, so six seconds sees a whole one.
  const Swing breathing = sweep(fixture.animator, "ParamBreath", 400);

  // Offset 0.5, peak 0.5, so it runs the whole 0..1 of the parameter.
  CHECK(breathing.range() > 0.5f);
}

TEST_CASE("live2d: a mouth follows what it was given") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);
  REQUIRE(!fixture.asset.lip_sync().empty());
  const nx::string mouth(fixture.asset.lip_sync()[0]);

  fixture.animator.update(1.f / 60.f);
  const f32 shut = fixture.animator.parameter(mouth.view());

  fixture.animator.set_mouth(1.f);
  fixture.animator.update(1.f / 60.f);
  const f32 open = fixture.animator.parameter(mouth.view());
  CHECK(open > shut);
  CHECK(open > 0.5f);

  // And closes again when the voice stops, rather than staying where the last
  // loud frame left it.
  fixture.animator.set_mouth(0.f);
  fixture.animator.update(1.f / 60.f);
  CHECK(fixture.animator.parameter(mouth.view()) < open);

  // Out of range is clamped rather than driving a parameter past its own.
  fixture.animator.set_mouth(5.f);
  CHECK(fixture.animator.mouth() == 1.f);
  fixture.animator.set_mouth(-2.f);
  CHECK(fixture.animator.mouth() == 0.f);
}

TEST_CASE("live2d: the effects reach the vertices, not just the parameters") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  fixture.animator.update(0.f);
  nx::vector<f32> rest;
  read_vertices(fixture.asset, rest);
  REQUIRE(!rest.empty());

  // Blink and breath together over two seconds. A parameter that moved but
  // never reached a deformer would pass every case above and show nothing.
  fixture.animator.set_blinking(true);
  fixture.animator.set_breathing(true);
  f32 worst = 0.f;
  nx::vector<f32> now;
  for (u32 i = 0; i < 120; ++i) {
    fixture.animator.update(1.f / 60.f);
    read_vertices(fixture.asset, now);
    for (usize v = 0; v < nx::min(rest.size(), now.size()); ++v)
      worst = nx::max(worst, std::fabs(now[v] - rest[v]));
  }
  CHECK(worst > 0.001f);
}
