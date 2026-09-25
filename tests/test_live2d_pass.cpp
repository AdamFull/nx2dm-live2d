
#include "framework/nxtest.h"

#include "fixture.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/vfs/vfs.h"
#include "live2d/live2d_animator.h"
#include "live2d/live2d_pass.h"
#include "rendering/pipeline_test_utils.h"

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
    mask = nxe::test::compile_pipeline(device, {
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
      models[i] = nxe::test::compile_pipeline(device, {
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

struct Box {
  glm::vec2 lo{0.f};
  glm::vec2 hi{0.f};
};

// Adds a model whose drawables are @p boxes, two triangles each, with its
// positions where the frame expects them. Returns its index.
u32 add_boxes(Frame &frame, const std::initializer_list<Box> boxes) {
  nx::shared_ptr<ModelMesh> mesh = nx::make_shared<ModelMesh>();
  mesh->first_vertex.push_back(0u);
  mesh->first_index.push_back(0u);
  FrameModel model;
  model.positions = nx::cast<u32>(frame.positions.size());
  for (const Box &box : boxes) {
    frame.positions.push_back(box.lo);
    frame.positions.push_back({box.hi.x, box.lo.y});
    frame.positions.push_back(box.hi);
    frame.positions.push_back({box.lo.x, box.hi.y});
    mesh->uvs.push_back({0.f, 0.f});
    mesh->uvs.push_back({1.f, 0.f});
    mesh->uvs.push_back({1.f, 1.f});
    mesh->uvs.push_back({0.f, 1.f});
    for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u})
      mesh->indices.push_back(i);
    mesh->first_vertex.push_back(nx::cast<u32>(mesh->uvs.size()));
    mesh->first_index.push_back(nx::cast<u32>(mesh->indices.size()));
  }
  model.mesh = mesh;
  frame.models.push_back(model);
  return nx::cast<u32>(frame.models.size() - 1u);
}

// A mesh draws from the frame after its upload lands. Draws @p frame once to
// start them, then waits for them.
void upload_meshes(rhi::Device &device, ModelRenderer &renderer,
                   const Frame &frame) {
  rg::RenderGraph graph;
  REQUIRE(graph.init(&device));
  graph.begin_frame();
  const rg::TextureId target = graph.create({
      .name = "live2d upload target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget,
  });
  renderer.draw(device, graph, target, rhi::Format::RGBA8_UNORM, 0, frame);
  graph.shutdown();
  device.uploader().flush();
  device.wait_idle();
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
  frame.models.push_back({.mesh = asset.mesh(), .world = view.world});
  frame.positions.resize(asset.mesh()->vertex_count());
  copy_positions(asset, frame.positions);
  REQUIRE(emit_draws(asset, view, &masks, 0, frame.draws) > 0u);
  REQUIRE(emit_mask_shapes(asset, masks, 0, frame.masks) > 0u);

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

  upload_meshes(device, renderer, frame);
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
  const u32 model = add_boxes(frame, {{{-0.5f, -0.5f}, {0.5f, 0.5f}}});
  frame.draws.push_back(
      {.model = model, .drawable = 0, .clip = {.group = 0, .atlas = 1}});
  frame.masks.push_back({.model = model, .drawable = 0, .atlas = 0});
  frame.masks.push_back({.model = model, .drawable = 0, .atlas = 1});
  frame.atlas_sizes = {64, 128};

  upload_meshes(device, renderer, frame);
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

TEST_CASE("live2d: the draw stream groups masks by atlas after the model") {
  Frame frame;
  const u32 model = add_boxes(frame, {{{0.f, 0.f}, {1.f, 1.f}},
                                      {{1.f, 0.f}, {2.f, 1.f}},
                                      {{2.f, 0.f}, {3.f, 1.f}}});
  frame.models[model].world[2] = glm::vec3(2.f, 3.f, 1.f);
  frame.models[model].camera = 4;
  frame.draws.push_back({.model = model,
                         .drawable = 0,
                         .color = 0x11223344u,
                         .clip = {.group = 0, .atlas = 1, .channel = 2}});
  frame.draws.push_back({.model = model, .drawable = 1});
  frame.draws.push_back({.model = model,
                         .drawable = 2,
                         .clip = {.group = 1, .atlas = 5, .inverted = true}});
  // Names a drawable the mesh does not have: dropped, not read past its end.
  frame.draws.push_back({.model = model, .drawable = 9});
  frame.masks.push_back({.model = model, .drawable = 1, .atlas = 1});
  frame.masks.push_back({.model = model, .drawable = 0, .atlas = 0});
  frame.masks.push_back({.model = model, .drawable = 2, .atlas = 1});
  frame.masks.push_back({.model = model, .drawable = 0, .atlas = 7});
  frame.atlas_sizes = {64, 128};

  static constexpr MeshAddress MESH[] = {{.uvs = 0x1000, .indices = 0x2000}};
  DrawStream stream;
  stream.build(frame, MESH);

  REQUIRE(stream.records.size() == 6u);
  REQUIRE(stream.commands.size() == 6u);
  CHECK(stream.model_count() == 3u);
  REQUIRE(stream.atlas_first.size() == 3u);
  CHECK(stream.atlas_first[0] == 3u);
  CHECK(stream.atlas_first[1] == 4u);
  CHECK(stream.atlas_first[2] == 6u);

  for (usize i = 0; i < stream.commands.size(); ++i) {
    CHECK(stream.commands[i].first_instance == i);
    CHECK(stream.commands[i].instance_count == 1u);
    CHECK(stream.commands[i].vertex_count == 6u);
    CHECK(stream.records[i].uvs == 0x1000u);
    CHECK(stream.records[i].indices == 0x2000u);
  }
  CHECK(stream.records[1].first_vertex == 4u);
  CHECK(stream.records[1].first_index == 6u);
  CHECK(stream.records[0].world0 == glm::vec4(1.f, 0.f, 2.f, 0.f));
  CHECK(stream.records[0].world1 == glm::vec4(0.f, 1.f, 3.f, 0.f));
  CHECK(stream.records[0].camera == 4u);
  CHECK(stream.records[0].color == 0x11223344u);
  CHECK(stream.records[0].mask == 1u);
  CHECK(stream.records[0].channel.z == 1.f);
  CHECK(stream.records[1].mask == NO_MASK);
  // Its atlas is past the frame's two, so it draws unclipped.
  CHECK(stream.records[2].mask == NO_MASK);
  CHECK(stream.records[2].inverted == 0u);

  CHECK(stream.records[3].first_index == 0u);
  CHECK(stream.records[4].first_index == 6u);
  CHECK(stream.records[5].first_index == 12u);
}

TEST_CASE("live2d: a model's mesh is uploaded once and let go when unused") {
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

  Frame first;
  add_boxes(first, {{{0.f, 0.f}, {1.f, 1.f}}});
  first.draws.push_back({.model = 0, .drawable = 0});
  Frame second;
  add_boxes(second, {{{0.f, 0.f}, {1.f, 1.f}}});
  second.draws.push_back({.model = 0, .drawable = 0});

  rg::RenderGraph graph;
  REQUIRE(graph.init(&device));
  const auto draw = [&](const Frame &frame) {
    graph.begin_frame();
    const rg::TextureId target = graph.create({
        .name = "live2d cache target",
        .format = rhi::Format::RGBA8_UNORM,
        .width = TARGET,
        .height = TARGET,
        .usage = rhi::TextureUsage::RenderTarget,
    });
    renderer.draw(device, graph, target, rhi::Format::RGBA8_UNORM, 0, frame);
  };

  draw(first);
  CHECK(graph.pass_count() == 0u);
  CHECK(renderer.cached_meshes() == 1u);
  device.uploader().flush();
  device.wait_idle();
  draw(first);
  CHECK(graph.pass_count() == 1u);
  CHECK(renderer.cached_meshes() == 1u);
  draw(second);
  CHECK(renderer.cached_meshes() == 2u);
  for (u32 i = 0; i < 12; ++i)
    draw(second);
  CHECK(renderer.cached_meshes() == 1u);
  // Nothing left on screen lets the last one go too.
  for (u32 i = 0; i < 12; ++i)
    draw(Frame{});
  CHECK(renderer.cached_meshes() == 0u);

  graph.shutdown();
  renderer.shutdown();
  pipelines.shutdown(device);
  device.destroy_shader(shader);
}

// Renders @p frame over black into a TARGET square whose camera maps 0..1 to
// the whole of it, once its meshes are resident, and reads it back.
nx::vector<u8> render(rhi::Device &device, ModelRenderer &renderer,
                      const Frame &frame, usize &passes) {
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

  upload_meshes(device, renderer, frame);
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
  passes = graph.pass_count();
  graph.compile();
  graph.execute(cmd);
  cmd.barrier(rhi::TextureBarrier{.texture = target,
                                  .from = rhi::ResourceState::ColorAttachment,
                                  .to = rhi::ResourceState::CopySrc});
  REQUIRE(device.end_headless_frame());
  device.wait_idle();

  const rhi::ReadbackResult read = device.uploader().read_texture(target);
  REQUIRE(read.data != nullptr);
  device.uploader().wait(read.ticket);
  nx::vector<u8> pixels(read.data,
                        read.data + nx::cast<usize>(TARGET) * TARGET * 4u);
  graph.shutdown();
  device.destroy_texture(target);
  device.destroy_buffer(cameras);
  return pixels;
}

// Red fills the left half of the target and green the right, each clipped by
// its own mask: red by atlas 0, green by atlas 1 on @p green_channel. The
// masks cover opposite halves of their atlases, so a draw that read another
// record, or a mask tile placed over its neighbour, lands in the wrong rows.
void check_masked_halves(const std::span<const u32> atlas_sizes,
                         const u32 green_channel, const usize expected_passes) {
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

  const u32 white = pack_texture(NX_TEXTURE_NONE, 0);

  // Drawables 0 and 1 are the halves; 2 and 3 are the masks, already in the
  // atlas's clip space.
  Frame frame;
  const u32 model = add_boxes(frame, {{{0.f, 0.f}, {0.5f, 1.f}},
                                      {{0.5f, 0.f}, {1.f, 1.f}},
                                      {{-1.f, 0.f}, {1.f, 1.f}},
                                      {{-1.f, -1.f}, {1.f, 0.f}}});
  // Each half stretches across its whole mask, as a Cubism clip does, so
  // sampling a shared atlas without the tile placement reads both tiles.
  glm::mat3 left(1.f);
  left[0][0] = 2.f;
  glm::mat3 right = left;
  right[2][0] = -1.f;
  frame.draws.push_back(
      {.model = model,
       .drawable = 0,
       .texture = white,
       .color = 0xff0000ffu,
       .clip = {.group = 0, .atlas = 0, .channel = 0, .to_mask = left}});
  frame.draws.push_back({.model = model,
                         .drawable = 1,
                         .texture = white,
                         .color = 0xff00ff00u,
                         .clip = {.group = 1,
                                  .atlas = 1,
                                  .channel = green_channel,
                                  .to_mask = right}});
  frame.masks.push_back({.model = model,
                         .drawable = 2,
                         .texture = white,
                         .atlas = 1,
                         .channel = green_channel});
  frame.masks.push_back({.model = model,
                         .drawable = 3,
                         .texture = white,
                         .atlas = 0,
                         .channel = 0});
  frame.atlas_sizes.assign(atlas_sizes.begin(), atlas_sizes.end());

  usize passes = 0;
  const nx::vector<u8> pixels = render(device, renderer, frame, passes);
  CHECK(passes == expected_passes);

  usize red_lit = 0, green_lit = 0, red_top = 0, green_top = 0, stray = 0;
  for (u32 y = 0; y < TARGET; ++y)
    for (u32 x = 0; x < TARGET; ++x) {
      const u8 *p = pixels.data() + (nx::cast<usize>(y) * TARGET + x) * 4;
      const bool r = p[0] > 128u && p[1] < 64u;
      const bool g = p[1] > 128u && p[0] < 64u;
      if (r && x < TARGET / 2) {
        ++red_lit;
        red_top += y < TARGET / 2 ? 1u : 0u;
      } else if (g && x >= TARGET / 2) {
        ++green_lit;
        green_top += y < TARGET / 2 ? 1u : 0u;
      } else if (r || g) {
        ++stray;
      }
    }
  const usize quarter = nx::cast<usize>(TARGET) * TARGET / 4;
  CHECK(stray == 0u);
  CHECK(red_lit > quarter * 9 / 10);
  CHECK(red_lit < quarter * 11 / 10);
  CHECK(green_lit > quarter * 9 / 10);
  CHECK(green_lit < quarter * 11 / 10);
  // Each colour sits wholly in one vertical half, and not the same one.
  CHECK((red_top == 0u || red_top == red_lit));
  CHECK((green_top == 0u || green_top == green_lit));
  CHECK((red_top == 0u) != (green_top == 0u));

  renderer.shutdown();
  pipelines.shutdown(device);
  device.destroy_shader(shader);
}

TEST_CASE("live2d: one multi-draw gives each draw its own mask") {
  // Different sizes never share an atlas: clear, two masks, the model.
  static constexpr u32 SIZES[] = {64, 128};
  check_masked_halves(SIZES, 1, 4);
}

TEST_CASE("live2d: masks sharing an atlas stay in their own tiles") {
  // One shared atlas and one channel, so only the tiles keep them apart.
  static constexpr u32 SIZES[] = {64, 64};
  check_masked_halves(SIZES, 0, 3);
}

TEST_CASE("live2d: mask atlases pack as tiles of shared atlases") {
  nx::small_vector<MaskTile, MAX_MASK_ATLASES> tiles;
  nx::small_vector<glm::uvec2, MAX_MASK_ATLASES> atlases;

  static constexpr u32 TWELVE[] = {512, 512, 512, 512, 512, 512,
                                   512, 512, 512, 512, 512, 512};
  pack_mask_atlases(TWELVE, 2048, tiles, atlases);
  REQUIRE(atlases.size() == 1u);
  CHECK(atlases[0] == glm::uvec2(2048, 1536));
  REQUIRE(tiles.size() == 12u);
  CHECK(tiles[0].scale == glm::vec2(0.25f, 1.f / 3.f));
  CHECK(tiles[0].offset == glm::vec2(0.f));
  CHECK(tiles[5].offset == glm::vec2(0.25f, 1.f / 3.f));
  CHECK(tiles[11].offset == glm::vec2(0.75f, 2.f / 3.f));

  // Five of them balance into three columns rather than four and a straggler.
  static constexpr u32 FIVE[] = {512, 512, 512, 512, 512};
  pack_mask_atlases(FIVE, 2048, tiles, atlases);
  REQUIRE(atlases.size() == 1u);
  CHECK(atlases[0] == glm::uvec2(1536, 1024));

  static constexpr u32 MIXED[] = {512, 256, 512};
  pack_mask_atlases(MIXED, 2048, tiles, atlases);
  REQUIRE(atlases.size() == 2u);
  CHECK(atlases[0] == glm::uvec2(1024, 512));
  CHECK(atlases[1] == glm::uvec2(256, 256));
  CHECK(tiles[0].atlas == 0u);
  CHECK(tiles[1].atlas == 1u);
  CHECK(tiles[2].atlas == 0u);
  CHECK(tiles[2].offset == glm::vec2(0.5f, 0.f));

  // A tile as big as the limit keeps an atlas to itself.
  static constexpr u32 FULL[] = {2048, 2048};
  pack_mask_atlases(FULL, 2048, tiles, atlases);
  REQUIRE(atlases.size() == 2u);
  CHECK(tiles[1].atlas == 1u);
  CHECK(tiles[1].scale == glm::vec2(1.f));
}

TEST_CASE(
    "live2d: each drawable samples its own uvs where its model is placed") {
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

  // A two-texel page: red on the left, green on the right.
  const rhi::TextureHandle page = device.create_texture({
      .name = "live2d page",
      .format = rhi::Format::RGBA8_UNORM,
      .width = 2,
      .height = 1,
      .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
  });
  REQUIRE(page.valid());
  static constexpr u8 TEXELS[] = {255, 0, 0, 255, 0, 255, 0, 255};
  const rhi::ImageSubresource whole = {
      .offset = 0, .size = 8, .width = 2, .height = 1, .row_pitch = 8};
  REQUIRE(device.uploader()
              .upload_texture(page, TEXELS, std::span(&whole, 1u))
              .ok());
  const rhi::SamplerHandle nearest = device.create_sampler({
      .name = "live2d nearest",
      .min_filter = rhi::FilterMode::Nearest,
      .mag_filter = rhi::FilterMode::Nearest,
      .mip_filter = rhi::FilterMode::Nearest,
  });
  REQUIRE(nearest.valid());
  const u32 texture =
      pack_texture(device.texture_index(page), device.sampler_index(nearest));

  // Two bands of one model, each sampling its own texel; the model is drawn
  // at half size in the middle of the target.
  Frame frame;
  add_boxes(frame, {{{0.f, 0.f}, {1.f, 0.5f}}, {{0.f, 0.5f}, {1.f, 1.f}}});
  nx::shared_ptr<ModelMesh> mesh =
      nx::make_shared<ModelMesh>(*frame.models[0].mesh);
  for (usize v = 0; v < mesh->uvs.size(); ++v)
    mesh->uvs[v] = {v < 4u ? 0.25f : 0.75f, 0.5f};
  frame.models[0].mesh = mesh;
  frame.models[0].world[0][0] = 0.5f;
  frame.models[0].world[1][1] = 0.5f;
  frame.models[0].world[2] = glm::vec3(0.25f, 0.25f, 1.f);
  for (u32 d = 0; d < 2u; ++d)
    frame.draws.push_back(
        {.model = 0, .drawable = d, .texture = texture, .color = 0xffffffffu});

  usize passes = 0;
  const nx::vector<u8> pixels = render(device, renderer, frame, passes);
  CHECK(passes == 2u);

  usize red = 0, green = 0, red_low = 0, green_low = 0, outside = 0;
  for (u32 y = 0; y < TARGET; ++y)
    for (u32 x = 0; x < TARGET; ++x) {
      const u8 *p = pixels.data() + (nx::cast<usize>(y) * TARGET + x) * 4;
      const bool r = p[0] > 128u && p[1] < 64u;
      const bool g = p[1] > 128u && p[0] < 64u;
      const bool centre = x >= TARGET / 4 && x < TARGET * 3 / 4 &&
                          y >= TARGET / 4 && y < TARGET * 3 / 4;
      if ((r || g) && !centre)
        ++outside;
      red += r ? 1u : 0u;
      green += g ? 1u : 0u;
      red_low += r && y < TARGET / 2 ? 1u : 0u;
      green_low += g && y < TARGET / 2 ? 1u : 0u;
    }
  const usize eighth = nx::cast<usize>(TARGET) * TARGET / 8;
  CHECK(outside == 0u);
  CHECK(red > eighth * 9 / 10);
  CHECK(red < eighth * 11 / 10);
  CHECK(green > eighth * 9 / 10);
  CHECK(green < eighth * 11 / 10);
  // One band each, and not the same one.
  CHECK((red_low == 0u || red_low == red));
  CHECK((green_low == 0u || green_low == green));
  CHECK((red_low == 0u) != (green_low == 0u));

  renderer.shutdown();
  pipelines.shutdown(device);
  device.destroy_shader(shader);
  device.destroy_texture(page);
}
