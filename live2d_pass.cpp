#include "live2d/live2d_pass.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/diagnostics/profiler.h"
#include "core/foundation/strings/format.h"

namespace nxm::live2d {
namespace {

namespace rhi = nxe::rhi;
namespace rg = nxe::rg;
namespace r2d = nxe::r2d;

} // namespace

rhi::BlendMode pipeline_blend(const r2d::MeshBlend blend) noexcept {
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

namespace {

void put_affine(DrawRecord &record, const glm::mat3 &m) noexcept {
  record.row0 = glm::vec4(m[0][0], m[1][0], m[2][0], 0.f);
  record.row1 = glm::vec4(m[0][1], m[1][1], m[2][1], 0.f);
}

[[nodiscard]] glm::vec4 channel_vector(const u32 channel) noexcept {
  glm::vec4 v(0.f);
  v[nx::cast<int>(nx::min(channel, 3u))] = 1.f;
  return v;
}

[[nodiscard]] rhi::DrawIndirectCommand command(const u32 index_count,
                                               const usize record) noexcept {
  return {.vertex_count = index_count,
          .instance_count = 1,
          .first_vertex = 0,
          .first_instance = nx::cast<u32>(record)};
}

// A tile's unit square in atlas UV is [offset, offset + scale]; in clip
// space, where the atlas spans -1..1, that is c' = scale * c + (scale - 1 +
// 2 * offset) per axis.
void place_in_uv(DrawRecord &record, const MaskTile &tile) noexcept {
  record.row0 *= tile.scale.x;
  record.row1 *= tile.scale.y;
  record.row0.z += tile.offset.x;
  record.row1.z += tile.offset.y;
}

void place_in_clip(DrawRecord &record, const MaskTile &tile) noexcept {
  const glm::vec2 shift = tile.scale - 1.f + 2.f * tile.offset;
  record.row0 *= tile.scale.x;
  record.row1 *= tile.scale.y;
  record.row0.z += shift.x;
  record.row1.z += shift.y;
  record.tile =
      glm::vec4(tile.scale, tile.scale) * record.tile + glm::vec4(shift, shift);
}

} // namespace

void pack_mask_atlases(
    const std::span<const u32> sizes, const u32 limit,
    nx::small_vector<MaskTile, MAX_MASK_ATLASES> &tiles,
    nx::small_vector<glm::uvec2, MAX_MASK_ATLASES> &atlases) {
  tiles.clear();
  atlases.clear();
  const usize count = nx::min(sizes.size(), nx::cast<usize>(MAX_MASK_ATLASES));
  tiles.resize(count);

  nx::small_vector<bool, MAX_MASK_ATLASES> placed(count, false);
  nx::small_vector<u32, MAX_MASK_ATLASES> members;
  for (usize first = 0; first < count; ++first) {
    if (placed[first])
      continue;
    const u32 size = nx::max(sizes[first], 1u);
    const u32 per_row = nx::max(limit / size, 1u);
    const u32 per_atlas = per_row * per_row;

    members.clear();
    for (usize i = first; i < count; ++i)
      if (!placed[i] && sizes[i] == sizes[first]) {
        placed[i] = true;
        members.push_back(nx::cast<u32>(i));
      }

    for (u32 start = 0; start < members.size(); start += per_atlas) {
      const u32 held =
          nx::min(nx::cast<u32>(members.size()) - start, per_atlas);
      const u32 rows = (held + per_row - 1u) / per_row;
      const u32 columns = (held + rows - 1u) / rows;
      const glm::uvec2 extent(columns * size, rows * size);
      const glm::vec2 span(extent);
      const u32 atlas = nx::cast<u32>(atlases.size());
      atlases.push_back(extent);
      for (u32 j = 0; j < held; ++j)
        tiles[members[start + j]] = {
            .atlas = atlas,
            .scale = glm::vec2(nx::cast<f32>(size)) / span,
            .offset = glm::vec2(nx::cast<f32>((j % columns) * size),
                                nx::cast<f32>((j / columns) * size)) /
                      span,
        };
    }
  }
}

void DrawStream::build(const Frame &frame, const u32 atlas_limit) {
  records.clear();
  commands.clear();
  atlas_first.clear();
  tiles.clear();
  atlases.clear();

  const bool masking = !frame.masks.empty() && !frame.atlas_sizes.empty();
  if (masking)
    pack_mask_atlases({frame.atlas_sizes.data(), frame.atlas_sizes.size()},
                      atlas_limit, tiles, atlases);
  const u32 logical = nx::cast<u32>(tiles.size());
  const u32 physical = nx::cast<u32>(atlases.size());

  const usize models = frame.geometry.draws.size();
  records.reserve(models + (masking ? frame.masks.draws.size() : 0u));
  commands.reserve(records.capacity());
  for (usize i = 0; i < models; ++i) {
    const r2d::MeshDraw &draw = frame.geometry.draws[i];
    const DrawMask &clip = i < frame.clips.size() ? frame.clips[i] : DrawMask{};
    DrawRecord &record = records.emplace_back();
    put_affine(record, clip.from_world);
    record.channel = channel_vector(clip.channel);
    record.index_offset = draw.first_index;
    record.vertex_offset = draw.vertex_offset;
    record.texture = nx_texture_2d<float4>(draw.texture);
    record.camera = draw.camera;
    const bool clipped = clip.clipped() && clip.atlas < logical;
    record.mask = clipped ? tiles[clip.atlas].atlas : NO_MASK;
    record.inverted = clipped && clip.inverted ? 1u : 0u;
    if (clipped)
      place_in_uv(record, tiles[clip.atlas]);
    commands.push_back(command(draw.index_count, i));
  }
  if (physical == 0u)
    return;

  // Masks only add, so grouping them by atlas cannot change what they draw.
  atlas_first.resize(physical + 1u, 0u);
  for (const MaskDraw &draw : frame.masks.draws)
    if (draw.atlas < logical)
      ++atlas_first[tiles[draw.atlas].atlas + 1u];
  atlas_first[0] = nx::cast<u32>(models);
  for (u32 a = 1; a <= physical; ++a)
    atlas_first[a] += atlas_first[a - 1u];

  const usize total = atlas_first[physical];
  records.resize(total);
  commands.resize(total);
  nx::small_vector<u32, MAX_MASK_ATLASES> next;
  for (u32 a = 0; a < physical; ++a)
    next.push_back(atlas_first[a]);
  for (const MaskDraw &draw : frame.masks.draws) {
    if (draw.atlas >= logical)
      continue;
    const u32 slot = next[tiles[draw.atlas].atlas]++;
    DrawRecord &record = records[slot];
    record = {};
    put_affine(record, draw.to_mask);
    record.channel = channel_vector(draw.channel);
    record.tile = draw.tile;
    record.index_offset = draw.first_index;
    record.vertex_offset = draw.vertex_offset;
    record.texture = nx_texture_2d<float4>(draw.texture);
    place_in_clip(record, tiles[draw.atlas]);
    commands[slot] = command(draw.index_count, slot);
  }
}

bool ModelRenderer::init(rhi::Device &device, const u32 sampler) {
  static constexpr nx::string_view ARRAYS[] = {
      "live2d vertices",     "live2d indices", "live2d mask vertices",
      "live2d mask indices", "live2d draws",   "live2d draw commands"};
  static_assert(nx::array_size(ARRAYS) == ARRAY_COUNT);
  if (!m_ring.init(&device, ARRAYS)) {
    return false;
  }
  m_sampler = sampler;
  return true;
}

void ModelRenderer::set_atlas_limit(const u32 limit) noexcept {
  m_atlas_limit = nx::max(limit, 1u);
}

void ModelRenderer::shutdown() {
  for (rhi::PipelineHandle &pipeline : m_model_pipeline)
    pipeline = {};
  m_mask_pipeline = {};
  m_format = rhi::Format::Unknown;
  m_ring.shutdown();
}

void ModelRenderer::set_pipelines(
    const rhi::PipelineHandle mask,
    const std::span<const rhi::PipelineHandle,
                    nx::cast<usize>(r2d::MeshBlend::Count)>
        models,
    const rhi::Format format) {
  m_mask_pipeline = mask;
  for (usize i = 0; i < models.size(); ++i)
    m_model_pipeline[i] = models[i];
  m_format = format;
}

void ModelRenderer::draw(rhi::Device &device, rg::RenderGraph &graph,
                         const rg::TextureId target, const rhi::Format format,
                         const u64 cameras, const Frame &frame) {
  NX_PROFILE_ZONE("live2d::draw");
  if (frame.empty() || !ready() || !target.valid())
    return;
  if (format != m_format)
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

  m_stream.build(frame, m_atlas_limit);
  const u64 draws =
      m_ring.write(DRAWS, m_stream.records.data(),
                   nx::cast<u64>(m_stream.records.size()) * sizeof(DrawRecord));
  if (draws == 0 || m_ring.write(COMMANDS, m_stream.commands.data(),
                                 nx::cast<u64>(m_stream.commands.size()) *
                                     sizeof(rhi::DrawIndirectCommand)) == 0)
    return;
  const rhi::BufferHandle commands = m_ring.buffer(COMMANDS);

  u64 mask_vertices = 0;
  u64 mask_indices = 0;
  nx::small_vector<rg::TextureId, MAX_MASK_ATLASES> atlases;
  if (!m_stream.atlas_first.empty()) {
    mask_vertices = m_ring.write(MASK_VERTICES, frame.masks.vertices.data(),
                                 nx::cast<u64>(frame.masks.vertices.size()) *
                                     sizeof(r2d::MeshVertex));
    mask_indices =
        m_ring.write(MASK_INDICES, frame.masks.indices.data(),
                     nx::cast<u64>(frame.masks.indices.size()) * sizeof(u32));
    if (mask_vertices == 0 || mask_indices == 0)
      return;

    for (const glm::uvec2 extent : m_stream.atlases)
      atlases.push_back(graph.create({
          .name = "live2d masks",
          .format = rhi::Format::RGBA8_UNORM,
          .width = extent.x,
          .height = extent.y,
          .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
      }));
  }

  for (usize atlas_index = 0; atlas_index < atlases.size(); ++atlas_index) {
    const rg::TextureId atlas = atlases[atlas_index];
    const glm::uvec2 extent = m_stream.atlases[atlas_index];
    const u32 first = m_stream.atlas_first[atlas_index];
    const u32 count = m_stream.atlas_first[atlas_index + 1u] - first;
    graph.add_pass(
        nx::format("live2d.masks.{}", atlas_index).view(),
        rg::SetupFn([atlas](rg::Builder &builder) {
          builder.color(0, atlas, rhi::clear_color(0.f, 0.f, 0.f, 0.f));
        }),
        rg::ExecuteFn([this, commands, mask_vertices, mask_indices, draws,
                       extent, first,
                       count](rhi::CommandContext &cmd, const rg::Resources &) {
          cmd.set_viewport({.width = nx::cast<f32>(extent.x),
                            .height = nx::cast<f32>(extent.y)});
          cmd.set_scissor({{0, 0}, {extent.x, extent.y}});
          if (count == 0u)
            return;
          cmd.bind_pipeline(m_mask_pipeline);
          PushBlock push;
          push.vertices = mask_vertices;
          push.indices = mask_indices;
          push.draws = draws;
          cmd.push_constants(&push, sizeof(push));
          for (u32 done = 0; done < count;) {
            const u32 run = nx::min(count - done, MAX_MULTI_DRAW);
            cmd.draw_indirect(commands,
                              nx::cast<u64>(first + done) *
                                  sizeof(rhi::DrawIndirectCommand),
                              run, sizeof(rhi::DrawIndirectCommand));
            done += run;
          }
        }));
  }

  graph.add_pass(
      "live2d.model", rg::SetupFn([target, atlases](rg::Builder &builder) {
        builder.color(0, target);
        for (const rg::TextureId atlas : atlases)
          builder.sample(atlas);
      }),
      rg::ExecuteFn([this, &device, &frame, atlases, commands, cameras,
                     vertices, indices, draws](rhi::CommandContext &cmd,
                                               const rg::Resources &resources) {
        PushBlock push;
        push.cameras = cameras;
        push.vertices = vertices;
        push.indices = indices;
        push.draws = draws;
        for (usize a = 0; a < atlases.size(); ++a)
          push.masks[a] = nx_texture_2d<float4>(pack_texture(
              device.texture_index(resources.texture(atlases[a])), m_sampler));

        // Each run of draws at one blend is one multi-draw, in draw order.
        const nx::vector<r2d::MeshDraw> &model = frame.geometry.draws;
        const u32 total = nx::cast<u32>(model.size());
        for (u32 i = 0; i < total;) {
          const r2d::MeshBlend blend = model[i].blend;
          u32 run = 1;
          while (i + run < total && run < MAX_MULTI_DRAW &&
                 model[i + run].blend == blend)
            ++run;
          cmd.bind_pipeline(m_model_pipeline[nx::cast<usize>(blend)]);
          cmd.push_constants(&push, sizeof(push));
          cmd.draw_indirect(commands,
                            nx::cast<u64>(i) * sizeof(rhi::DrawIndirectCommand),
                            run, sizeof(rhi::DrawIndirectCommand));
          i += run;
        }
      }));
}

} // namespace nxm::live2d
