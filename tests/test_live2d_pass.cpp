
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_pass.h"

#include <cstring>

namespace {

using namespace nxm::live2d;
using namespace nxm::live2d_test;

namespace r2d = nxe::r2d;
namespace rhi = nxe::rhi;
namespace rg = nxe::rg;

constexpr u32 TARGET = 256;
constexpr u32 ATLAS = 512;

struct TestDevice {
  rhi::Device device;
  bool ready = false;

  TestDevice() {
    rhi::DeviceDesc desc{};
    desc.application_name = "nx live2d pass tests";
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

struct TestPipelines {
  rhi::PipelineHandle mask;
  rhi::PipelineHandle models[nx::cast<usize>(r2d::MeshBlend::Count)] = {};

  [[nodiscard]] bool init(rhi::Device &device, const rhi::ShaderHandle shader,
                          const rhi::Format format) {
    mask = device.create_graphics_pipeline({
        .name = "live2d mask test",
        .vertex = {.shader = shader, .entry_point = "mask_vs"},
        .fragment = {.shader = shader, .entry_point = "mask_fs"},
        .color_formats = {rhi::Format::RGBA8_UNORM},
        .color_count = 1,
        .blend = {{.enabled = true,
                   .mode = rhi::BlendMode::PremultipliedAdditive}},
    });
    if (!mask.valid())
      return false;
    for (usize i = 0; i < nx::array_size(models); ++i) {
      models[i] = device.create_graphics_pipeline({
          .name = "live2d model test",
          .vertex = {.shader = shader, .entry_point = "model_vs"},
          .fragment = {.shader = shader, .entry_point = "model_fs"},
          .color_formats = {format},
          .color_count = 1,
          .blend = {{.enabled = true,
                     .mode = pipeline_blend(nx::cast<r2d::MeshBlend>(i))}},
      });
      if (!models[i].valid())
        return false;
    }
    return true;
  }

  void shutdown(rhi::Device &device) noexcept {
    for (const rhi::PipelineHandle pipeline : models)
      if (pipeline.valid())
        device.destroy_pipeline(pipeline);
    if (mask.valid())
      device.destroy_pipeline(mask);
    mask = {};
    for (rhi::PipelineHandle &pipeline : models)
      pipeline = {};
  }
};

[[nodiscard]] usize lit(const nx::vector<u8> &pixels) noexcept {
  usize n = 0;
  for (usize i = 0; i < nx::cast<usize>(TARGET) * TARGET; ++i)
    if (pixels[i * 4] > 8u)
      ++n;
  return n;
}

} // namespace

TEST_CASE("live2d: the renderer draws a masked model through the graph") {
  NX_REQUIRE_FIXTURE();
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_live2d_shader(device);
  if (!shader.valid())
    SKIP("the module's shaders are not built in this configuration");

  const rhi::SamplerHandle sampler = device.create_sampler({
      .name = "live2d mask",
      .address_u = rhi::AddressMode::ClampToEdge,
      .address_v = rhi::AddressMode::ClampToEdge,
  });
  REQUIRE(sampler.valid());

  ModelRenderer renderer;
  REQUIRE(renderer.init(device, device.sampler_index(sampler)));
  TestPipelines pipelines;
  REQUIRE(pipelines.init(device, shader, rhi::Format::RGBA8_UNORM));
  renderer.set_pipelines(pipelines.mask, pipelines.models,
                         rhi::Format::RGBA8_UNORM);
  CHECK(renderer.ready());

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

  Frame frame;
  frame.atlas_sizes.push_back(ATLAS);
  REQUIRE(emit_model(asset, view, masks, frame.geometry, frame.clips) > 0u);
  REQUIRE(emit_masks(asset, masks, frame.masks) > 0u);

  GpuCamera2D camera = {};
  glm::mat4 proj(1.f);
  proj[0][0] = 2.f;
  proj[1][1] = 2.f;
  proj[3][0] = -1.f;
  proj[3][1] = -1.f;
  camera.view_proj = proj;
  const rhi::BufferHandle cameras = device.create_buffer({
      .name = "live2d cameras",
      .size = sizeof(camera),
      .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::DeviceAddress,
      .memory = rhi::MemoryUsage::Upload,
      .persistently_mapped = true,
  });
  REQUIRE(cameras.valid());
  std::memcpy(device.buffer_mapped(cameras), &camera, sizeof(camera));

  const rhi::TextureHandle target = device.create_texture({
      .name = "live2d pass target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled |
               rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  rg::RenderGraph graph;
  REQUIRE(graph.init(&device));

  rhi::CommandContext cmd;
  REQUIRE(device.begin_headless_frame(cmd));
  graph.begin_frame();

  const rg::TextureId imported = graph.import(
      nx::id_string::from_literal("live2d target"), target,
      rhi::Extent2D{TARGET, TARGET}, rhi::ResourceState::Undefined);
  graph.add_pass(
      "clear", rg::SetupFn([imported](rg::Builder &builder) {
        builder.color(0, imported, rhi::clear_color(0.f, 0.f, 0.f, 1.f));
      }),
      rg::ExecuteFn([](rhi::CommandContext &, const rg::Resources &) {}));

  renderer.draw(device, graph, imported, rhi::Format::RGBA8_UNORM,
                device.buffer_address(cameras), frame);

  CHECK(graph.pass_count() >= 3u);

  graph.compile();
  graph.execute(cmd);
  cmd.barrier(rhi::TextureBarrier{.texture = target,
                                  .from = rhi::ResourceState::ColorAttachment,
                                  .to = rhi::ResourceState::CopySrc});
  REQUIRE(device.end_headless_frame());
  device.wait_idle();

  const rhi::ReadbackResult pixels = device.uploader().read_texture(target);
  REQUIRE(pixels.data != nullptr);
  device.uploader().wait(pixels.ticket);
  nx::vector<u8> frame_pixels(
      pixels.data, pixels.data + nx::cast<usize>(TARGET) * TARGET * 4);

  const usize covered = lit(frame_pixels);
  CHECK(covered > (TARGET * TARGET) / 20);
  CHECK(covered < (TARGET * TARGET * 4) / 5);

  graph.shutdown();
  renderer.shutdown();
  pipelines.shutdown(device);
  device.destroy_shader(shader);
  device.destroy_texture(target);
  device.destroy_buffer(cameras);
  asset = ModelAsset();
  nx::vfs::shutdown();
}

TEST_CASE("live2d: a renderer with nothing to draw records no passes") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_live2d_shader(device);
  if (!shader.valid())
    SKIP("the module's shaders are not built in this configuration");

  ModelRenderer renderer;
  REQUIRE(renderer.init(device, 0));

  rg::RenderGraph graph;
  REQUIRE(graph.init(&device));
  graph.begin_frame();

  const Frame nothing;
  renderer.draw(device, graph, {}, rhi::Format::RGBA8_UNORM, 0, nothing);
  CHECK(graph.pass_count() == 0u);

  graph.shutdown();
  renderer.shutdown();
  device.destroy_shader(shader);
}

TEST_CASE("live2d: the renderer records every mask atlas") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_live2d_shader(device);
  if (!shader.valid())
    SKIP("the module's shaders are not built in this configuration");

  ModelRenderer renderer;
  REQUIRE(renderer.init(device, 0));
  TestPipelines pipelines;
  REQUIRE(pipelines.init(device, shader, rhi::Format::RGBA8_UNORM));
  renderer.set_pipelines(pipelines.mask, pipelines.models,
                         rhi::Format::RGBA8_UNORM);

  Frame frame;
  frame.geometry.vertices = {
      {{-0.5f, -0.5f}, {0.f, 0.f}, 0xffff'ffffu},
      {{0.5f, -0.5f}, {1.f, 0.f}, 0xffff'ffffu},
      {{0.f, 0.5f}, {0.5f, 1.f}, 0xffff'ffffu},
  };
  frame.geometry.indices = {0, 1, 2};
  frame.geometry.draws.push_back({.index_count = 3});
  frame.clips.push_back({.group = 0, .atlas = 1});
  frame.masks.vertices = frame.geometry.vertices;
  frame.masks.indices = frame.geometry.indices;
  frame.masks.draws.push_back({.index_count = 3, .atlas = 0});
  frame.masks.draws.push_back({.index_count = 3, .atlas = 1});
  frame.atlas_sizes = {64, 128};

  rg::RenderGraph graph;
  REQUIRE(graph.init(&device));
  graph.begin_frame();
  const rg::TextureId target = graph.create({
      .name = "live2d multi-atlas target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget,
  });
  renderer.draw(device, graph, target, rhi::Format::RGBA8_UNORM, 0, frame);

  CHECK(graph.pass_count() == 3u);

  graph.shutdown();
  renderer.shutdown();
  pipelines.shutdown(device);
  device.destroy_shader(shader);
}
