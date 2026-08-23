
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/vfs/vfs.h"
#include "core/rendering/rhi/image/image.h"
#include "core/rendering/rhi/rhi.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_draw.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdlib>
#include <cstring>

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;

namespace r2d = nxe::r2d;
namespace rhi = nxe::rhi;

constexpr u32 TARGET = 256;

struct MeshPush {
  u64 cameras = 0;
  u64 vertices = 0;
  u64 indices = 0;
  u64 materials = 0;
  ::MeshPushFields fields = {};
};

struct TestDevice {
  rhi::Device device;
  bool ready = false;

  TestDevice() {
    rhi::DeviceDesc desc{};
    desc.application_name = "nx live2d tests";
    ready = device.init(desc);
  }
  ~TestDevice() {
    if (ready)
      device.shutdown();
  }
  TestDevice(const TestDevice &) = delete;
  TestDevice &operator=(const TestDevice &) = delete;
};

[[nodiscard]] rhi::ShaderHandle load_mesh_shader(rhi::Device &device) {
  const nx::string base = nx::string(NX_TEST_SHADER_DIR) + "/mesh";
  auto code = nx::fs::file_read(nx::fs::path_view(base + ".spv"));
  auto refl = nx::fs::file_read_text(nx::fs::path_view(base + ".refl.json"));
  if (!code.has_value() || !refl.has_value())
    return {};
  return device.create_shader({
      .name = "mesh",
      .code = code->data(),
      .code_size = code->size(),
      .reflection_json = refl->view(),
  });
}

[[nodiscard]] usize lit(const u8 *const pixels) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (pixels[i * 4] != 0u || pixels[i * 4 + 1] != 0u ||
        pixels[i * 4 + 2] != 0u)
      ++n;
  return n;
}

[[nodiscard]] usize differing(const u8 *const a, const u8 *const b) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (std::memcmp(a + i * 4, b + i * 4, 3) != 0)
      ++n;
  return n;
}

[[nodiscard]] usize painted(const nx::vector<u8> &pixels) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (pixels[i * 4] > 8u || pixels[i * 4 + 1] > 8u || pixels[i * 4 + 2] > 8u)
      ++n;
  return n;
}

void shrink(rhi::ImageData &image, const u32 max_side) {
  const u32 side = nx::max(image.width, image.height);
  const u32 step = side > max_side ? side / max_side : 1u;
  if (step <= 1u)
    return;

  const u32 w = image.width / step;
  const u32 h = image.height / step;
  u8 *const p = image.pixels.data();
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x) {
      const usize from =
          (nx::cast<usize>(y) * step * image.width + nx::cast<usize>(x) * step) *
          4;
      const usize to = (nx::cast<usize>(y) * w + x) * 4;
      std::memcpy(p + to, p + from, 4);
    }

  image.width = w;
  image.height = h;
  image.mip_levels = 1;
  image.subresources.resize(1);
  image.subresources[0] = {.offset = 0,
                           .size = nx::cast<u64>(w) * h * 4,
                           .mip = 0,
                           .layer = 0,
                           .width = w,
                           .height = h,
                           .depth = 1,
                           .row_pitch = w * 4};
}

void dump(const u8 *const pixels, const u32 pass) {
  const char *const dir = std::getenv("NX_LIVE2D_DUMP");
  if (dir == nullptr)
    return;
  const nx::string path = nx::format("{}/live2d_frame{}.png", dir, pass);
  (void)stbi_write_png(path.c_str(), nx::cast<int>(TARGET),
                       nx::cast<int>(TARGET), 4, pixels,
                       nx::cast<int>(TARGET) * 4);
}

}

TEST_CASE("live2d: a model reaches the framebuffer, and driving it changes "
          "what is on it") {
  NX_REQUIRE_FIXTURE();
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_mesh_shader(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  nx::vfs::initialize();
  nx::vfs::Device *const host =
      nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
  REQUIRE(host != nullptr);
  nx::vfs::mount("/", host);

  ModelAsset asset;
  nx::string error;
  REQUIRE(load_model(MODEL, {}, asset, error));
  Animator animator(asset);

  const rhi::TextureHandle target = device.create_texture({
      .name = "live2d target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  const rhi::PipelineHandle pipeline = device.create_graphics_pipeline({
      .name = "mesh",
      .vertex = {.shader = shader, .entry_point = "vs_main"},
      .fragment = {.shader = shader, .entry_point = "fs_main"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
      .blend = {{.enabled = true, .mode = rhi::BlendMode::AlphaBlend}},
  });
  REQUIRE(pipeline.valid());

  GpuCamera2D camera = {};
  glm::mat4 proj(1.f);
  proj[0][0] = 2.f;
  proj[1][1] = 2.f;
  proj[3][0] = -1.f;
  proj[3][1] = -1.f;
  camera.view_proj = proj;

  ModelView view;
  view.world[0][0] = 0.8f;
  view.world[1][1] = 0.8f;
  view.world[2][0] = 0.5f;
  view.world[2][1] = 0.52f;

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

  nx::vector<u8> frames[2];
  for (u32 pass = 0; pass < 2; ++pass) {
    REQUIRE(animator.set_parameter("ParamAngleX", pass == 0 ? 0.f : 30.f));
    animator.refresh();

    r2d::MeshChannel channel;
    REQUIRE(emit_model(asset, view, channel) > 0u);
    REQUIRE(!channel.vertices.empty());

    const rhi::BufferHandle vertices = upload(
        "live2d vertices", channel.vertices.data(),
        nx::cast<u64>(channel.vertices.size()) * sizeof(r2d::MeshVertex));
    const rhi::BufferHandle indices =
        upload("live2d indices", channel.indices.data(),
               nx::cast<u64>(channel.indices.size()) * sizeof(u32));

    MeshPush push;
    push.cameras = device.buffer_address(cameras);
    push.vertices = device.buffer_address(vertices);
    push.indices = device.buffer_address(indices);

    rhi::CommandContext cmd;
    REQUIRE(device.begin_headless_frame(cmd));
    cmd.barrier(rhi::TextureBarrier{.texture = target,
                                    .from = rhi::ResourceState::Undefined,
                                    .to = rhi::ResourceState::ColorAttachment});
    rhi::RenderPassDesc render = {};
    render.name = "live2d";
    render.color[0].texture = target;
    render.color[0].load = rhi::LoadOp::Clear;
    render.color[0].store = rhi::StoreOp::Store;
    render.color[0].clear = rhi::clear_color(0.f, 0.f, 0.f, 1.f);
    render.color_count = 1;
    cmd.begin_render_pass(render);
    cmd.set_viewport(
        {.width = nx::cast<f32>(TARGET), .height = nx::cast<f32>(TARGET)});
    cmd.set_scissor({{0, 0}, {TARGET, TARGET}});
    cmd.bind_pipeline(pipeline);
    for (const r2d::MeshDraw &draw : channel.draws) {
      push.fields.index_offset = draw.first_index;
      push.fields.vertex_offset = draw.vertex_offset;
      push.fields.texture = draw.texture;
      push.fields.camera = draw.camera;
      cmd.push_constants(&push, sizeof(push));
      cmd.draw(draw.index_count);
    }
    cmd.end_render_pass();
    cmd.barrier(rhi::TextureBarrier{.texture = target,
                                    .from = rhi::ResourceState::ColorAttachment,
                                    .to = rhi::ResourceState::CopySrc});
    REQUIRE(device.end_headless_frame());
    device.wait_idle();

    const rhi::ReadbackResult pixels = device.uploader().read_texture(target);
    REQUIRE(pixels.data != nullptr);
    device.uploader().wait(pixels.ticket);
    frames[pass].assign(pixels.data,
                        pixels.data + nx::cast<usize>(TARGET) * TARGET * 4);
    dump(frames[pass].data(), pass);

    device.destroy_buffer(indices);
    device.destroy_buffer(vertices);
  }

  const usize covered = lit(frames[0].data());
  CHECK(covered > (TARGET * TARGET) / 20);
  CHECK(covered < (TARGET * TARGET * 4) / 5);

  CHECK(differing(frames[0].data(), frames[1].data()) > covered / 50);

  device.destroy_buffer(cameras);
  device.destroy_pipeline(pipeline);
  device.destroy_texture(target);
  device.destroy_shader(shader);
  asset = ModelAsset();
  nx::vfs::shutdown();
}

TEST_CASE("live2d: a model's own pages land on it, not on the empty half of "
          "the atlas") {
  NX_REQUIRE_FIXTURE();
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_mesh_shader(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  nx::vfs::initialize();
  nx::vfs::Device *const host =
      nx::vfs::make_host_device(nx::fs::path_view(fixture_dir()));
  REQUIRE(host != nullptr);
  nx::vfs::mount("/", host);

  const rhi::SamplerHandle sampler = device.create_sampler({
      .name = "live2d page",
      .address_u = rhi::AddressMode::ClampToEdge,
      .address_v = rhi::AddressMode::ClampToEdge,
  });
  REQUIRE(sampler.valid());

  nx::vector<rhi::TextureHandle> pages;
  const auto resolve = [&](const nx::string_view path) -> u32 {
    const auto bytes = nx::vfs::read(path);
    if (!bytes)
      return pack_texture(NX_TEXTURE_NONE, 0);
    auto image = rhi::load_image_from_memory(
        std::span<const u8>(bytes->data(), bytes->size()));
    if (!image || image->format != rhi::Format::RGBA8_UNORM)
      return pack_texture(NX_TEXTURE_NONE, 0);

    shrink(*image, 2048);
    const rhi::TextureHandle page =
        device.create_texture(image->texture_desc(path));
    if (!page.valid())
      return pack_texture(NX_TEXTURE_NONE, 0);
    if (!device.uploader().upload_texture(page, *image).ok()) {
      device.destroy_texture(page);
      return pack_texture(NX_TEXTURE_NONE, 0);
    }
    pages.push_back(page);
    return pack_texture(device.texture_index(page),
                        device.sampler_index(sampler));
  };

  nx::string error;
  ModelAsset asset;
  REQUIRE(load_model(MODEL, TextureResolver(resolve), asset, error));
  if (pages.empty())
    SKIP("the fixture's pages are not RGBA8 images this can upload");
  for (const u32 page : asset.textures())
    REQUIRE((page >> 16) != NX_TEXTURE_NONE);

  Animator animator(asset);
  animator.update(0.f);

  const rhi::TextureHandle target = device.create_texture({
      .name = "live2d target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  const rhi::PipelineHandle pipeline = device.create_graphics_pipeline({
      .name = "mesh",
      .vertex = {.shader = shader, .entry_point = "vs_main"},
      .fragment = {.shader = shader, .entry_point = "fs_main"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
      .blend = {{.enabled = true, .mode = rhi::BlendMode::AlphaBlend}},
  });
  REQUIRE(pipeline.valid());

  GpuCamera2D camera = {};
  glm::mat4 proj(1.f);
  proj[0][0] = 2.f;
  proj[1][1] = 2.f;
  proj[3][0] = -1.f;
  proj[3][1] = -1.f;
  camera.view_proj = proj;

  ModelView view;
  view.world[0][0] = 0.8f;
  view.world[1][1] = 0.8f;
  view.world[2][0] = 0.5f;
  view.world[2][1] = 0.52f;

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

  r2d::MeshChannel channel;
  REQUIRE(emit_model(asset, view, channel) > 0u);
  const rhi::BufferHandle vertices =
      upload("live2d vertices", channel.vertices.data(),
             nx::cast<u64>(channel.vertices.size()) * sizeof(r2d::MeshVertex));
  const rhi::BufferHandle indices =
      upload("live2d indices", channel.indices.data(),
             nx::cast<u64>(channel.indices.size()) * sizeof(u32));

  MeshPush push;
  push.cameras = device.buffer_address(cameras);
  push.vertices = device.buffer_address(vertices);
  push.indices = device.buffer_address(indices);

  enum Pass : u32 { Pages, Silhouette, PassCount };
  nx::vector<u8> frames[PassCount];

  for (u32 pass = 0; pass < PassCount; ++pass) {
    rhi::CommandContext cmd;
    REQUIRE(device.begin_headless_frame(cmd));
    cmd.barrier(rhi::TextureBarrier{.texture = target,
                                    .from = rhi::ResourceState::Undefined,
                                    .to = rhi::ResourceState::ColorAttachment});
    rhi::RenderPassDesc render = {};
    render.name = "live2d pages";
    render.color[0].texture = target;
    render.color[0].load = rhi::LoadOp::Clear;
    render.color[0].store = rhi::StoreOp::Store;
    render.color[0].clear = rhi::clear_color(0.f, 0.f, 0.f, 1.f);
    render.color_count = 1;
    cmd.begin_render_pass(render);
    cmd.set_viewport(
        {.width = nx::cast<f32>(TARGET), .height = nx::cast<f32>(TARGET)});
    cmd.set_scissor({{0, 0}, {TARGET, TARGET}});
    cmd.bind_pipeline(pipeline);
    for (const r2d::MeshDraw &draw : channel.draws) {
      push.fields.index_offset = draw.first_index;
      push.fields.vertex_offset = draw.vertex_offset;
      push.fields.texture = pass == Pages ? draw.texture
                                          : pack_texture(NX_TEXTURE_NONE, 0);
      push.fields.camera = draw.camera;
      cmd.push_constants(&push, sizeof(push));
      cmd.draw(draw.index_count);
    }
    cmd.end_render_pass();
    cmd.barrier(rhi::TextureBarrier{.texture = target,
                                    .from = rhi::ResourceState::ColorAttachment,
                                    .to = rhi::ResourceState::CopySrc});
    REQUIRE(device.end_headless_frame());
    device.wait_idle();

    const rhi::ReadbackResult pixels = device.uploader().read_texture(target);
    REQUIRE(pixels.data != nullptr);
    device.uploader().wait(pixels.ticket);
    frames[pass].assign(pixels.data,
                        pixels.data + nx::cast<usize>(TARGET) * TARGET * 4);
    dump(frames[pass].data(), 2u + pass);
  }

  const usize shape = painted(frames[Silhouette]);
  const usize art = painted(frames[Pages]);
  REQUIRE(shape > (TARGET * TARGET) / 20);

  CHECK(art * 5 > shape * 3);

  for (const rhi::TextureHandle page : pages)
    device.destroy_texture(page);
  device.destroy_buffer(indices);
  device.destroy_buffer(vertices);
  device.destroy_buffer(cameras);
  device.destroy_pipeline(pipeline);
  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_shader(shader);
  asset = ModelAsset();
  nx::vfs::shutdown();
}
