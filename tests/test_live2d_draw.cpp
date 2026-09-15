
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"
#include "rendering/render2d/render_interop.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_draw.h"

#include <cmath>

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;
namespace r2d = nxe::r2d;

constexpr u32 PAGE = 7;

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
    ok = load_model(MODEL, TextureResolver([](nx::string_view) {
                      return pack_texture(PAGE, 0);
                    }),
                    asset, error);
    animator.bind(&asset);
    animator.update(0.f);
  }
  ~Loaded() {
    asset = ModelAsset();
    nx::vfs::shutdown();
  }

  Loaded(const Loaded &) = delete;
  Loaded &operator=(const Loaded &) = delete;
};

[[nodiscard]] f32 emitted_agreement(const r2d::MeshChannel &channel) {
  f64 weighted = 0.0;
  f64 total = 0.0;
  for (const r2d::MeshDraw &draw : channel.draws) {
    const usize first = draw.vertex_offset;
    usize last = channel.vertices.size();
    for (const r2d::MeshDraw &other : channel.draws)
      if (other.vertex_offset > first)
        last = nx::min(last, nx::cast<usize>(other.vertex_offset));
    if (last <= first + 2)
      continue;

    f64 mean_y = 0.0;
    f64 mean_v = 0.0;
    const usize n = last - first;
    for (usize i = first; i < last; ++i) {
      mean_y += channel.vertices[i].position.y;
      mean_v += channel.vertices[i].uv.y;
    }
    mean_y /= nx::cast<f64>(n);
    mean_v /= nx::cast<f64>(n);

    f64 covariance = 0.0;
    f64 var_y = 0.0;
    f64 var_v = 0.0;
    for (usize i = first; i < last; ++i) {
      const f64 dy = channel.vertices[i].position.y - mean_y;
      const f64 dv = channel.vertices[i].uv.y - mean_v;
      covariance += dy * dv;
      var_y += dy * dy;
      var_v += dv * dv;
    }
    if (var_y <= 0.0 || var_v <= 0.0)
      continue;
    weighted += (covariance / std::sqrt(var_y * var_v)) * nx::cast<f64>(n);
    total += nx::cast<f64>(n);
  }
  return total > 0.0 ? nx::cast<f32>(weighted / total) : 0.f;
}

}

TEST_CASE("live2d: the emitted geometry keeps Cubism's texture convention") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  r2d::MeshChannel channel;
  REQUIRE(emit_model(fixture.asset, {}, channel) > 0u);

  CHECK(emitted_agreement(channel) < -0.5f);
}

TEST_CASE("live2d: the view's material rides every emitted draw") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  ModelView view;
  view.batch = 5u;
  view.material = 8u;
  r2d::MeshChannel channel;
  REQUIRE(emit_model(fixture.asset, view, channel) > 0u);
  for (const r2d::MeshDraw &draw : channel.draws) {
    CHECK(draw.batch == 5u);
    CHECK(draw.material == 8u);
  }
}

TEST_CASE("live2d: a posed model becomes mesh draws in render order") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  r2d::MeshChannel channel;
  const usize draws = emit_model(fixture.asset, {}, channel);
  REQUIRE(draws > 0u);
  CHECK(channel.draws.size() == draws);
  CHECK(!channel.vertices.empty());
  CHECK(!channel.indices.empty());

  CHECK(draws <= fixture.asset.drawable_count());

  for (const r2d::MeshDraw &draw : channel.draws) {
    CHECK(draw.index_count > 0u);
    CHECK(draw.first_index + draw.index_count <= channel.indices.size());
    CHECK(draw.vertex_offset <= channel.vertices.size());
    for (u32 i = 0; i < draw.index_count; ++i) {
      const u32 index = channel.indices[draw.first_index + i];
      CHECK(draw.vertex_offset + index < channel.vertices.size());
    }
  }

  const u32 key = channel.draws[0].sort_key;
  for (const r2d::MeshDraw &draw : channel.draws)
    CHECK(draw.sort_key == key);
}

TEST_CASE("live2d: the texture a drawable names is the one the manifest "
          "resolved") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  r2d::MeshChannel channel;
  REQUIRE(emit_model(fixture.asset, {}, channel) > 0u);

  for (const r2d::MeshDraw &draw : channel.draws) {
    CHECK((draw.texture >> 16) == PAGE);
    CHECK((draw.texture >> 16) != NX_TEXTURE_NONE);
  }
}

TEST_CASE("live2d: the world transform reaches every vertex") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  r2d::MeshChannel at_origin;
  REQUIRE(emit_model(fixture.asset, {}, at_origin) > 0u);

  ModelView view;
  view.world[0][0] = 100.f;
  view.world[1][1] = 100.f;
  view.world[2][0] = 5.f;
  view.world[2][1] = -3.f;
  r2d::MeshChannel placed;
  REQUIRE(emit_model(fixture.asset, view, placed) > 0u);

  REQUIRE(placed.vertices.size() == at_origin.vertices.size());
  for (usize i = 0; i < placed.vertices.size(); ++i) {
    const glm::vec2 &before = at_origin.vertices[i].position;
    const glm::vec2 &after = placed.vertices[i].position;
    CHECK(std::fabs(after.x - (before.x * 100.f + 5.f)) < 1e-3f);
    CHECK(std::fabs(after.y - (before.y * 100.f - 3.f)) < 1e-3f);
  }
}

TEST_CASE("live2d: opacity and tint reach the vertex colour") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  r2d::MeshChannel plain;
  REQUIRE(emit_model(fixture.asset, {}, plain) > 0u);

  ModelView half;
  half.color = glm::vec4(1.f, 1.f, 1.f, 0.5f);
  r2d::MeshChannel faded;
  REQUIRE(emit_model(fixture.asset, half, faded) > 0u);

  REQUIRE(faded.vertices.size() == plain.vertices.size());
  usize dimmed = 0;
  for (usize i = 0; i < faded.vertices.size(); ++i) {
    const u32 was = plain.vertices[i].color >> 24;
    const u32 now = faded.vertices[i].color >> 24;
    CHECK(now <= was);
    dimmed += now < was ? 1u : 0u;
  }
  CHECK(dimmed > faded.vertices.size() / 2u);
}

TEST_CASE("live2d: an unloaded model emits nothing rather than crashing") {
  ModelAsset empty;
  r2d::MeshChannel channel;
  CHECK(emit_model(empty, {}, channel) == 0u);
  CHECK(channel.empty());
  CHECK(masked_drawable_count(empty) == 0u);
}

TEST_CASE("live2d: the drawables this path cannot clip are counted") {
  NX_REQUIRE_FIXTURE();
  Loaded fixture;
  REQUIRE(fixture.ok);

  const usize masked = masked_drawable_count(fixture.asset);
  CHECK(masked <= fixture.asset.drawable_count());
  nx::logi("live2d: {} of {} visible drawables would be clipped", masked,
           fixture.asset.drawable_count());
}
