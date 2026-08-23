#include "live2d/live2d_pass.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/strings/format.h"

namespace nxm::live2d {
namespace {

namespace rhi = nxe::rhi;
namespace rg = nxe::rg;
namespace r2d = nxe::r2d;

[[nodiscard]] rhi::BlendMode blend_of(const r2d::MeshBlend blend) noexcept {
  switch (blend) {
  case r2d::MeshBlend::NormalPremultiplied:
    return rhi::BlendMode::Premultiplied;
  case r2d::MeshBlend::Additive:
    return rhi::BlendMode::Additive;
  case r2d::MeshBlend::AdditivePremultiplied:
    return rhi::BlendMode::PremultipliedAdditive;
  case r2d::MeshBlend::Multiply:
    return rhi::BlendMode::Multiply;
  case r2d::MeshBlend::Screen:
    return rhi::BlendMode::Screen;
  case r2d::MeshBlend::Normal:
  case r2d::MeshBlend::Count:
    break;
  }
  return rhi::BlendMode::AlphaBlend;
}

void put_affine(PushBlock &push, const glm::mat3 &m) noexcept {
  push.mask_row0 = glm::vec4(m[0][0], m[1][0], m[2][0], 0.f);
  push.mask_row1 = glm::vec4(m[0][1], m[1][1], m[2][1], 0.f);
}

[[nodiscard]] glm::vec4 channel_vector(const u32 channel) noexcept {
  glm::vec4 v(0.f);
  v[nx::cast<int>(nx::min(channel, 3u))] = 1.f;
  return v;
}

}

bool ModelRenderer::init(rhi::Device &device, const rhi::ShaderHandle shader,
                         const u32 sampler) {
  if (!shader.valid()) {
    nx::loge("live2d: no shader; the model will not draw");
    return false;
  }
  static constexpr nx::string_view ARRAYS[] = {
      "live2d vertices", "live2d indices", "live2d mask vertices",
      "live2d mask indices"};
  static_assert(nx::array_size(ARRAYS) == ARRAY_COUNT);
  if (!m_ring.init(&device, ARRAYS))
    return false;

  m_shader = shader;
  m_sampler = sampler;
  return true;
}

void ModelRenderer::shutdown(rhi::Device &device) {
  for (rhi::PipelineHandle &pipeline : m_model_pipeline) {
    if (pipeline.valid())
      device.destroy_pipeline(pipeline);
    pipeline = {};
  }
  if (m_mask_pipeline.valid())
    device.destroy_pipeline(m_mask_pipeline);
  m_mask_pipeline = {};
  if (m_shader.valid())
    device.destroy_shader(m_shader);
  m_shader = {};
  m_format = rhi::Format::Unknown;
  m_ring.shutdown();
}

bool ModelRenderer::ensure_pipelines(rhi::Device &device,
                                     const rhi::Format format) {
  if (format == m_format && m_mask_pipeline.valid())
    return true;

  for (rhi::PipelineHandle &pipeline : m_model_pipeline) {
    if (pipeline.valid())
      device.destroy_pipeline(pipeline);
    pipeline = {};
  }
  if (m_mask_pipeline.valid())
    device.destroy_pipeline(m_mask_pipeline);

  // The atlas is always RGBA8 whatever the scene is: it holds four masks, not
  // a picture, and eight bits of coverage is what Cubism's own renderers use.
  m_mask_pipeline = device.create_graphics_pipeline({
      .name = "live2d mask",
      .vertex = {.shader = m_shader, .entry_point = "mask_vs"},
      .fragment = {.shader = m_shader, .entry_point = "mask_fs"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
      .blend = {{.enabled = true,
                 .mode = rhi::BlendMode::PremultipliedAdditive}},
  });
  if (!m_mask_pipeline.valid())
    return false;

  for (usize i = 0; i < nx::cast<usize>(r2d::MeshBlend::Count); ++i) {
    m_model_pipeline[i] = device.create_graphics_pipeline({
        .name = "live2d model",
        .vertex = {.shader = m_shader, .entry_point = "model_vs"},
        .fragment = {.shader = m_shader, .entry_point = "model_fs"},
        .color_formats = {format},
        .color_count = 1,
        .blend = {{.enabled = true,
                   .mode = blend_of(nx::cast<r2d::MeshBlend>(i))}},
    });
    if (!m_model_pipeline[i].valid())
      return false;
  }
  m_format = format;
  return true;
}

void ModelRenderer::draw(rhi::Device &device, rg::RenderGraph &graph,
                         const rg::TextureId target, const rhi::Format format,
                         const u64 cameras, const Frame &frame) {
  if (frame.empty() || !ready() || !target.valid())
    return;
  if (!ensure_pipelines(device, format))
    return;

  m_ring.next_frame();
  const u64 vertices = m_ring.write(
      MODEL_VERTICES, frame.geometry.vertices.data(),
      nx::cast<u64>(frame.geometry.vertices.size()) * sizeof(r2d::MeshVertex));
  const u64 indices =
      m_ring.write(MODEL_INDICES, frame.geometry.indices.data(),
                   nx::cast<u64>(frame.geometry.indices.size()) * sizeof(u32));
  if (vertices == 0 || indices == 0)
    return;

  const bool masking = !frame.masks.empty() && !frame.atlas_sizes.empty();
  u64 mask_vertices = 0;
  u64 mask_indices = 0;
  nx::small_vector<rg::TextureId, 16> atlases;
  if (masking) {
    mask_vertices = m_ring.write(MASK_VERTICES, frame.masks.vertices.data(),
                                 nx::cast<u64>(frame.masks.vertices.size()) *
                                     sizeof(r2d::MeshVertex));
    mask_indices =
        m_ring.write(MASK_INDICES, frame.masks.indices.data(),
                     nx::cast<u64>(frame.masks.indices.size()) * sizeof(u32));
    if (mask_vertices == 0 || mask_indices == 0)
      return;

    atlases.reserve(frame.atlas_sizes.size());
    for (const u32 size : frame.atlas_sizes)
      atlases.push_back(graph.create({
          .name = "live2d masks",
          .format = rhi::Format::RGBA8_UNORM,
          .width = size,
          .height = size,
          .usage =
              rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
      }));
  }

  if (masking) {
    for (usize atlas_index = 0; atlas_index < atlases.size(); ++atlas_index) {
      const rg::TextureId atlas = atlases[atlas_index];
      const u32 size = frame.atlas_sizes[atlas_index];
      graph.add_pass(
          nx::format("live2d.masks.{}", atlas_index).view(),
          rg::SetupFn([atlas](rg::Builder &builder) {
            builder.color(0, atlas, rhi::clear_color(0.f, 0.f, 0.f, 0.f));
          }),
          rg::ExecuteFn(
              [this, &frame, mask_vertices, mask_indices, size,
               atlas_index](rhi::CommandContext &cmd, const rg::Resources &) {
                cmd.set_viewport({.width = nx::cast<f32>(size),
                                  .height = nx::cast<f32>(size)});
                cmd.set_scissor({{0, 0}, {size, size}});
                cmd.bind_pipeline(m_mask_pipeline);
                for (const MaskDraw &draw : frame.masks.draws) {
                  if (nx::cast<usize>(draw.atlas) != atlas_index)
                    continue;
                  PushBlock push;
                  put_affine(push, draw.to_mask);
                  push.channel = channel_vector(draw.channel);
                  push.tile = draw.tile;
                  push.vertices = mask_vertices;
                  push.indices = mask_indices;
                  push.index_offset = draw.first_index;
                  push.vertex_offset = draw.vertex_offset;
                  push.texture = draw.texture;
                  cmd.push_constants(&push, sizeof(push));
                  cmd.draw(draw.index_count);
                }
              }));
    }
  }

  graph.add_pass(
      "live2d.model",
      rg::SetupFn([target, atlases](rg::Builder &builder) {
        builder.color(0, target);
        for (const rg::TextureId atlas : atlases)
          builder.sample(atlas);
      }),
      rg::ExecuteFn([this, &device, &frame, atlases, cameras, vertices,
                     indices](rhi::CommandContext &cmd,
                              const rg::Resources &resources) {
        nx::small_vector<u32, 16> mask_textures;
        mask_textures.reserve(atlases.size());
        for (const rg::TextureId atlas : atlases)
          mask_textures.push_back(pack_texture(
              device.texture_index(resources.texture(atlas)), m_sampler));

        r2d::MeshBlend bound = r2d::MeshBlend::Count;
        for (usize i = 0; i < frame.geometry.draws.size(); ++i) {
          const r2d::MeshDraw &draw = frame.geometry.draws[i];
          if (draw.blend != bound) {
            bound = draw.blend;
            cmd.bind_pipeline(m_model_pipeline[nx::cast<usize>(bound)]);
          }

          const DrawMask &clip =
              i < frame.clips.size() ? frame.clips[i] : DrawMask{};
          PushBlock push;
          put_affine(push, clip.from_world);
          push.channel = channel_vector(clip.channel);
          push.cameras = cameras;
          push.vertices = vertices;
          push.indices = indices;
          push.index_offset = draw.first_index;
          push.vertex_offset = draw.vertex_offset;
          push.texture = draw.texture;
          push.camera = draw.camera;
          const bool clipped =
              clip.clipped() &&
              nx::cast<usize>(clip.atlas) < mask_textures.size();
          push.mask_texture = clipped
                                  ? mask_textures[clip.atlas]
                                  : pack_texture(NX_TEXTURE_NONE, 0);
          push.inverted = clipped && clip.inverted ? 1u : 0u;
          cmd.push_constants(&push, sizeof(push));
          cmd.draw(draw.index_count);
        }
      }));
}

}
