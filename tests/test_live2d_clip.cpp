
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/vfs.h"
#include "rendering/pipeline_test_utils.h"
#include "rendering/rhi/rhi.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_draw.h"
#include "live2d/live2d_mask.h"

#include <cstdlib>
#include <cstring>

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;

namespace r2d = nxe::r2d;
namespace rhi = nxe::rhi;

constexpr u32 TARGET = 256;
constexpr u32 ATLAS = 512;

struct Live2DPush {
  glm::vec4 mask_row0{0.f};
  glm::vec4 mask_row1{0.f};
  glm::vec4 channel{0.f};
  glm::vec4 tile{-1.f, -1.f, 1.f, 1.f};
  u64 cameras = 0;
  u64 vertices = 0;
  u64 indices = 0;
  u32 index_offset = 0;
  u32 vertex_offset = 0;
  NxTexture2D<float4> texture{};
  u32 camera = 0;
  NxTexture2D<float4> mask_texture{};
  u32 inverted = 0;
};
static_assert(sizeof(Live2DPush) == 112);
static_assert(sizeof(Live2DPush) <= 128, "the guaranteed Vulkan push range");

struct TestDevice {
  rhi::Device device;
  bool ready = false;

  TestDevice() {
    rhi::DeviceDesc desc{};
    desc.application_name = "nx live2d clip tests";
    ready = device.init(desc);
  }
  ~TestDevice() {
    if (ready)
      device.shutdown();
  }
  TestDevice(const TestDevice &) = delete;
  TestDevice &operator=(const TestDevice &) = delete;
};

[[nodiscard]] rhi::ShaderHandle load_live2d_shader(rhi::Device &device) {
  const nx::string base = nx::string(NX_TEST_SHADER_DIR) + "/live2d/live2d";
  auto code = nx::fs::file_read(nx::fs::path_view(base + ".spv"));
  auto refl = nx::fs::file_read_text(nx::fs::path_view(base + ".refl.json"));
  if (!code.has_value() || !refl.has_value())
    return {};
  return device.create_shader({
      .name = "live2d",
      .code = code->data(),
      .code_size = code->size(),
      .reflection_json = refl->view(),
  });
}

void put_affine(Live2DPush &push, const glm::mat3 &m) noexcept {
  push.mask_row0 = glm::vec4(m[0][0], m[1][0], m[2][0], 0.f);
  push.mask_row1 = glm::vec4(m[0][1], m[1][1], m[2][1], 0.f);
}

[[nodiscard]] glm::vec4 channel_vector(const u32 channel) noexcept {
  glm::vec4 v(0.f);
  v[nx::cast<int>(nx::min(channel, 3u))] = 1.f;
  return v;
}

[[nodiscard]] usize lit(const nx::vector<u8> &pixels) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (pixels[i * 4] > 8u)
      ++n;
  return n;
}

[[nodiscard]] usize both(const nx::vector<u8> &a,
                         const nx::vector<u8> &b) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (a[i * 4] > 8u && b[i * 4] > 8u)
      ++n;
  return n;
}

[[nodiscard]] usize either(const nx::vector<u8> &a,
                           const nx::vector<u8> &b) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (a[i * 4] > 8u || b[i * 4] > 8u)
      ++n;
  return n;
}

} // namespace

TEST_CASE("live2d: a mask keeps what it covers, and its inverse keeps the "
          "rest") {
  NX_REQUIRE_FIXTURE();
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_live2d_shader(device);
  if (!shader.valid())
    SKIP("the module's shaders are not built in this configuration");

  nx::vfs::initialize();
  nx::vfs::Device *const host =
      nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
  REQUIRE(host != nullptr);
  nx::vfs::mount("/", host);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));
  Animator animator(asset);
  MaskLayout masks;
  REQUIRE(masks.build(asset, ATLAS, 1));
  animator.update(0.f);
  masks.update(asset);
  REQUIRE(masks.active());

  ModelView view;
  view.world[0][0] = 0.8f;
  view.world[1][1] = 0.8f;
  view.world[2][0] = 0.5f;
  view.world[2][1] = 0.52f;

  r2d::MeshChannel model;
  nx::vector<DrawMask> clips;
  REQUIRE(emit_model(asset, view, masks, model, clips) > 0u);
  REQUIRE(clips.size() == model.draws.size());

  MaskChannel shapes;
  REQUIRE(emit_masks(asset, masks, shapes) > 0u);

  usize subject = model.draws.size();
  for (usize i = 0; i < clips.size(); ++i)
    if (clips[i].clipped() && model.draws[i].index_count > 60u) {
      subject = i;
      break;
    }
  REQUIRE(subject < model.draws.size());

  const rhi::TextureHandle atlas = device.create_texture({
      .name = "live2d masks",
      .format = rhi::Format::RGBA8_UNORM,
      .width = ATLAS,
      .height = ATLAS,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
               rhi::TextureUsage::CopySrc,
  });
  REQUIRE(atlas.valid());
  const rhi::TextureHandle target = device.create_texture({
      .name = "live2d clip target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  const rhi::PipelineHandle mask_pipeline = nxe::test::compile_pipeline(device, {
      .name = "live2d mask",
      .vertex = {.shader = shader, .entry_point = "mask_vs"},
      .fragment = {.shader = shader, .entry_point = "mask_fs"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
      .blend = {{.enabled = true,
                 .mode = rhi::BlendMode::PremultipliedAdditive}},
  });
  REQUIRE(mask_pipeline.valid());

  const rhi::PipelineHandle model_pipeline = nxe::test::compile_pipeline(device, {
      .name = "live2d model",
      .vertex = {.shader = shader, .entry_point = "model_vs"},
      .fragment = {.shader = shader, .entry_point = "model_fs"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
      .blend = {{.enabled = true, .mode = rhi::BlendMode::AlphaBlend}},
  });
  REQUIRE(model_pipeline.valid());

  GpuCamera2D camera = {};
  glm::mat4 proj(1.f);
  proj[0][0] = 2.f;
  proj[1][1] = 2.f;
  proj[3][0] = -1.f;
  proj[3][1] = -1.f;
  camera.view_proj = proj;

  const auto upload = [&](const nx::string_view name, const void *const data,
                          const u64 bytes) {
    const rhi::BufferHandle b = device.create_buffer({
        .name = name,
        .size = bytes,
        .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::DeviceAddress,
        .memory = rhi::MemoryUsage::Upload,
        .persistently_mapped = true,
    });
    REQUIRE(b.valid());
    std::memcpy(device.buffer_mapped(b), data, bytes);
    return b;
  };
  const rhi::BufferHandle cameras =
      upload("live2d cameras", &camera, sizeof(camera));
  const rhi::BufferHandle model_vertices =
      upload("live2d model vertices", model.vertices.data(),
             nx::cast<u64>(model.vertices.size()) * sizeof(r2d::MeshVertex));
  const rhi::BufferHandle model_indices =
      upload("live2d model indices", model.indices.data(),
             nx::cast<u64>(model.indices.size()) * sizeof(u32));
  const rhi::BufferHandle mask_vertices =
      upload("live2d mask vertices", shapes.vertices.data(),
             nx::cast<u64>(shapes.vertices.size()) * sizeof(r2d::MeshVertex));
  const rhi::BufferHandle mask_indices =
      upload("live2d mask indices", shapes.indices.data(),
             nx::cast<u64>(shapes.indices.size()) * sizeof(u32));

  const rhi::SamplerHandle sampler = device.create_sampler({
      .name = "live2d mask",
      .address_u = rhi::AddressMode::ClampToEdge,
      .address_v = rhi::AddressMode::ClampToEdge,
  });
  REQUIRE(sampler.valid());
  const u32 atlas_packed =
      pack_texture(device.texture_index(atlas), device.sampler_index(sampler));
  REQUIRE((atlas_packed >> 16) != NX_TEXTURE_NONE);
  REQUIRE((atlas_packed & 0xFFFFu) != 0xFFFFu);

  enum Pass : u32 { Unclipped, Clipped, Inverted, PassCount };
  nx::vector<u8> frames[PassCount];
  usize covered[4] = {};

  for (u32 pass = 0; pass < PassCount; ++pass) {
    rhi::CommandContext cmd;
    REQUIRE(device.begin_headless_frame(cmd));

    cmd.barrier(rhi::TextureBarrier{.texture = atlas,
                                    .from = rhi::ResourceState::Undefined,
                                    .to = rhi::ResourceState::ColorAttachment});
    rhi::RenderPassDesc mask_pass = {};
    mask_pass.name = "live2d masks";
    mask_pass.color[0].texture = atlas;
    mask_pass.color[0].load = rhi::LoadOp::Clear;
    mask_pass.color[0].store = rhi::StoreOp::Store;
    mask_pass.color[0].clear = rhi::clear_color(0.f, 0.f, 0.f, 0.f);
    mask_pass.color_count = 1;
    cmd.begin_render_pass(mask_pass);
    cmd.set_viewport(
        {.width = nx::cast<f32>(ATLAS), .height = nx::cast<f32>(ATLAS)});
    cmd.set_scissor({{0, 0}, {ATLAS, ATLAS}});
    cmd.bind_pipeline(mask_pipeline);
    for (const MaskDraw &draw : shapes.draws) {
      Live2DPush push;
      put_affine(push, draw.to_mask);
      push.channel = channel_vector(draw.channel);
      push.tile = draw.tile;
      push.vertices = device.buffer_address(mask_vertices);
      push.indices = device.buffer_address(mask_indices);
      push.index_offset = draw.first_index;
      push.vertex_offset = draw.vertex_offset;
      push.texture = nx_texture_2d<float4>(draw.texture);
      cmd.push_constants(&push, sizeof(push));
      cmd.draw(draw.index_count);
    }
    cmd.end_render_pass();
    cmd.barrier(rhi::TextureBarrier{.texture = atlas,
                                    .from = rhi::ResourceState::ColorAttachment,
                                    .to = rhi::ResourceState::ShaderReadOnly});

    cmd.barrier(rhi::TextureBarrier{.texture = target,
                                    .from = rhi::ResourceState::Undefined,
                                    .to = rhi::ResourceState::ColorAttachment});
    rhi::RenderPassDesc draw_pass = {};
    draw_pass.name = "live2d model";
    draw_pass.color[0].texture = target;
    draw_pass.color[0].load = rhi::LoadOp::Clear;
    draw_pass.color[0].store = rhi::StoreOp::Store;
    draw_pass.color[0].clear = rhi::clear_color(0.f, 0.f, 0.f, 1.f);
    draw_pass.color_count = 1;
    cmd.begin_render_pass(draw_pass);
    cmd.set_viewport(
        {.width = nx::cast<f32>(TARGET), .height = nx::cast<f32>(TARGET)});
    cmd.set_scissor({{0, 0}, {TARGET, TARGET}});
    cmd.bind_pipeline(model_pipeline);
    {
      const r2d::MeshDraw &draw = model.draws[subject];
      const DrawMask &clip = clips[subject];
      Live2DPush push;
      put_affine(push, clip.from_world);
      push.channel = channel_vector(clip.channel);
      push.cameras = device.buffer_address(cameras);
      push.vertices = device.buffer_address(model_vertices);
      push.indices = device.buffer_address(model_indices);
      push.index_offset = draw.first_index;
      push.vertex_offset = draw.vertex_offset;
      push.texture = nx_texture_2d<float4>(draw.texture);
      push.camera = draw.camera;
      push.mask_texture = nx_texture_2d<float4>(
          pass == Unclipped ? pack_texture(NX_TEXTURE_NONE, 0) : atlas_packed);
      push.inverted = pass == Inverted ? 1u : 0u;
      cmd.push_constants(&push, sizeof(push));
      cmd.draw(draw.index_count);
    }
    cmd.end_render_pass();
    cmd.barrier(rhi::TextureBarrier{.texture = target,
                                    .from = rhi::ResourceState::ColorAttachment,
                                    .to = rhi::ResourceState::CopySrc});
    REQUIRE(device.end_headless_frame());
    device.wait_idle();

    if (pass == Clipped) {
      const rhi::ReadbackResult mask = device.uploader().read_texture(atlas);
      REQUIRE(mask.data != nullptr);
      device.uploader().wait(mask.ticket);
      for (usize c = 0; c < 4; ++c)
        for (usize i = 0; i < nx::cast<usize>(ATLAS) * ATLAS; ++i)
          covered[c] += mask.data[i * 4 + c] > 8u ? 1u : 0u;
    }

    const rhi::ReadbackResult pixels = device.uploader().read_texture(target);
    REQUIRE(pixels.data != nullptr);
    device.uploader().wait(pixels.ticket);
    frames[pass].assign(pixels.data,
                        pixels.data + nx::cast<usize>(TARGET) * TARGET * 4);

    if (const char *const dir = std::getenv("NX_LIVE2D_DUMP"); dir != nullptr) {
      const nx::string path = nx::format("{}/live2d_clip{}.raw", dir, pass);
      (void)nx::fs::file_write(
          nx::fs::path_view(path),
          std::span<const u8>(frames[pass].data(), frames[pass].size()));
    }
  }

  bool used[4] = {};
  for (const MaskGroup &group : masks.groups())
    used[nx::min(group.channel, 3u)] = true;
  for (usize c = 0; c < 4; ++c) {
    if (used[c])
      CHECK(covered[c] > 0u);
    else
      CHECK(covered[c] == 0u);
  }

  const usize whole = lit(frames[Unclipped]);
  const usize kept = lit(frames[Clipped]);
  const usize dropped = lit(frames[Inverted]);
  REQUIRE(whole > 200u);

  CHECK(kept > 0u);
  CHECK(kept < whole);
  CHECK(dropped > 0u);
  CHECK(dropped < whole);

  CHECK(either(frames[Clipped], frames[Inverted]) > whole * 95u / 100u);
  CHECK(both(frames[Clipped], frames[Inverted]) < whole / 10u);

  device.destroy_buffer(mask_indices);
  device.destroy_buffer(mask_vertices);
  device.destroy_buffer(model_indices);
  device.destroy_buffer(model_vertices);
  device.destroy_buffer(cameras);
  device.destroy_pipeline(model_pipeline);
  device.destroy_pipeline(mask_pipeline);
  device.destroy_texture(target);
  device.destroy_texture(atlas);
  device.destroy_shader(shader);
  asset = ModelAsset();
  nx::vfs::shutdown();
}
