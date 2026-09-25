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

namespace {

// The drawable's place in its model's mesh and the frame's positions.
void place_drawable(DrawRecord &record, const FrameModel &model,
                    const MeshAddress &address, const u32 drawable) noexcept {
  const ModelMesh &mesh = *model.mesh;
  record.uvs = address.uvs;
  record.indices = address.indices;
  record.first_index = mesh.first_index[drawable];
  record.first_vertex = mesh.first_vertex[drawable];
  record.positions = model.positions;
}

[[nodiscard]] u32 index_count(const FrameModel &model,
                              const u32 drawable) noexcept {
  return model.mesh->first_index[drawable + 1u] -
         model.mesh->first_index[drawable];
}

[[nodiscard]] bool names_drawable(const Frame &frame, const u32 model,
                                  const u32 drawable) noexcept {
  return model < frame.models.size() && frame.models[model].mesh != nullptr &&
         drawable < frame.models[model].mesh->drawable_count();
}

[[nodiscard]] bool resident(const std::span<const MeshAddress> meshes,
                            const u32 model) noexcept {
  return model < meshes.size() && meshes[model].resident();
}

} // namespace

void DrawStream::build(const Frame &frame,
                       const std::span<const MeshAddress> meshes,
                       const u32 atlas_limit) {
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
  const auto drawable = [&](const u32 model, const u32 index) {
    return resident(meshes, model) && names_drawable(frame, model, index);
  };

  records.reserve(frame.draws.size() + (masking ? frame.masks.size() : 0u));
  commands.reserve(records.capacity());
  for (const ModelDraw &draw : frame.draws) {
    if (!drawable(draw.model, draw.drawable))
      continue;
    const FrameModel &model = frame.models[draw.model];
    const usize slot = records.size();
    DrawRecord &record = records.emplace_back();
    place_drawable(record, model, meshes[draw.model], draw.drawable);
    put_affine(record, draw.clip.to_mask);
    record.world0 =
        glm::vec4(model.world[0][0], model.world[1][0], model.world[2][0], 0.f);
    record.world1 =
        glm::vec4(model.world[0][1], model.world[1][1], model.world[2][1], 0.f);
    record.channel = channel_vector(draw.clip.channel);
    record.texture = nx_texture_2d<float4>(draw.texture);
    record.camera = model.camera;
    record.color = draw.color;
    const bool clipped = draw.clip.clipped() && draw.clip.atlas < logical;
    record.mask = clipped ? tiles[draw.clip.atlas].atlas : NO_MASK;
    record.inverted = clipped && draw.clip.inverted ? 1u : 0u;
    if (clipped)
      place_in_uv(record, tiles[draw.clip.atlas]);
    commands.push_back(command(index_count(model, draw.drawable), slot));
  }
  if (physical == 0u)
    return;

  // Masks only add, so grouping them by atlas cannot change what they draw.
  const u32 models = nx::cast<u32>(records.size());
  atlas_first.resize(physical + 1u, 0u);
  for (const MaskShape &shape : frame.masks)
    if (shape.atlas < logical && drawable(shape.model, shape.drawable))
      ++atlas_first[tiles[shape.atlas].atlas + 1u];
  atlas_first[0] = models;
  for (u32 a = 1; a <= physical; ++a)
    atlas_first[a] += atlas_first[a - 1u];

  const usize total = atlas_first[physical];
  records.resize(total);
  commands.resize(total);
  nx::small_vector<u32, MAX_MASK_ATLASES> next;
  for (u32 a = 0; a < physical; ++a)
    next.push_back(atlas_first[a]);
  for (const MaskShape &shape : frame.masks) {
    if (shape.atlas >= logical || !drawable(shape.model, shape.drawable))
      continue;
    const FrameModel &model = frame.models[shape.model];
    const u32 slot = next[tiles[shape.atlas].atlas]++;
    DrawRecord &record = records[slot];
    record = {};
    place_drawable(record, model, meshes[shape.model], shape.drawable);
    put_affine(record, shape.to_mask);
    record.channel = channel_vector(shape.channel);
    record.tile = shape.tile;
    record.texture = nx_texture_2d<float4>(shape.texture);
    place_in_clip(record, tiles[shape.atlas]);
    commands[slot] = command(index_count(model, shape.drawable), slot);
  }
}

bool ModelRenderer::init(rhi::Device &device, const u32 sampler) {
  static constexpr nx::string_view ARRAYS[] = {
      "live2d positions", "live2d draws", "live2d draw commands"};
  static_assert(nx::array_size(ARRAYS) == ARRAY_COUNT);
  if (!m_ring.init(&device, ARRAYS)) {
    return false;
  }
  m_device = &device;
  m_sampler = sampler;
  return true;
}

void ModelRenderer::set_atlas_limit(const u32 limit) noexcept {
  m_atlas_limit = nx::max(limit, 1u);
}

void ModelRenderer::release(CachedMesh &cached) noexcept {
  if (m_device != nullptr) {
    if (cached.uvs.valid())
      m_device->destroy_buffer(cached.uvs);
    if (cached.indices.valid())
      m_device->destroy_buffer(cached.indices);
  }
  cached = {};
}

void ModelRenderer::release_unused() noexcept {
  for (usize i = 0; i < m_meshes.size();) {
    if (m_meshes[i].used + KEEP_FRAMES >= m_frame) {
      ++i;
      continue;
    }
    release(m_meshes[i]);
    m_meshes[i] = std::move(m_meshes.back());
    m_meshes.pop_back();
  }
}

MeshAddress
ModelRenderer::resolve(const nx::shared_ptr<const ModelMesh> &mesh) {
  if (mesh == nullptr || m_device == nullptr)
    return {};
  rhi::Uploader &uploader = m_device->uploader();
  // Published only once its copy has landed: until then a frame's submission
  // need not wait for it, and the model draws nothing.
  const auto publish = [&](CachedMesh &cached) {
    if (!cached.resident)
      cached.resident = uploader.is_complete(cached.uv_upload) &&
                        uploader.is_complete(cached.index_upload);
    return cached.resident ? cached.address : MeshAddress{};
  };
  for (CachedMesh &cached : m_meshes)
    if (cached.mesh.get() == mesh.get()) {
      cached.used = m_frame;
      return publish(cached);
    }

  // Device local: written once when the model first draws, then read by every
  // vertex of every frame it is on screen.
  const auto upload = [&](const nx::string_view name, const void *const data,
                          const u64 bytes, rhi::UploadTicket &ticket) {
    const rhi::BufferHandle buffer = m_device->create_buffer({
        .name = name,
        .size = nx::max<u64>(bytes, 16u),
        .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::DeviceAddress |
                 rhi::BufferUsage::CopyDst,
        .memory = rhi::MemoryUsage::DeviceLocal,
    });
    if (!buffer.valid())
      return buffer;
    ticket = uploader.upload_buffer(
        buffer, 0, {static_cast<const u8 *>(data), nx::cast<usize>(bytes)});
    if (!ticket.ok()) {
      m_device->destroy_buffer(buffer);
      return rhi::BufferHandle{};
    }
    return buffer;
  };

  CachedMesh cached;
  cached.mesh = mesh;
  cached.used = m_frame;
  cached.uvs = upload("live2d uvs", mesh->uvs.data(),
                      nx::cast<u64>(mesh->uvs.size()) * sizeof(glm::vec2),
                      cached.uv_upload);
  cached.indices = upload("live2d indices", mesh->indices.data(),
                          nx::cast<u64>(mesh->indices.size()) * sizeof(u32),
                          cached.index_upload);
  if (!cached.uvs.valid() || !cached.indices.valid()) {
    release(cached);
    return {};
  }
  cached.address = {m_device->buffer_address(cached.uvs),
                    m_device->buffer_address(cached.indices)};
  m_meshes.push_back(std::move(cached));
  return publish(m_meshes.back());
}

void ModelRenderer::shutdown() {
  for (CachedMesh &cached : m_meshes)
    release(cached);
  m_meshes.clear();
  for (rhi::PipelineHandle &pipeline : m_model_pipeline)
    pipeline = {};
  m_mask_pipeline = {};
  m_format = rhi::Format::Unknown;
  m_ring.shutdown();
  m_device = nullptr;
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
  ++m_frame;
  release_unused();
  if (frame.empty() || !ready() || !target.valid())
    return;
  if (format != m_format)
    return;

  m_addresses.clear();
  for (const FrameModel &model : frame.models)
    m_addresses.push_back(resolve(model.mesh));

  m_ring.next_frame();
  const u64 positions =
      m_ring.write(POSITIONS, frame.positions.data(),
                   nx::cast<u64>(frame.positions.size()) * sizeof(glm::vec2));
  if (positions == 0)
    return;

  m_stream.build(frame, {m_addresses.data(), m_addresses.size()},
                 m_atlas_limit);
  if (m_stream.records.empty())
    return;
  const u64 draws =
      m_ring.write(DRAWS, m_stream.records.data(),
                   nx::cast<u64>(m_stream.records.size()) * sizeof(DrawRecord));
  if (draws == 0 || m_ring.write(COMMANDS, m_stream.commands.data(),
                                 nx::cast<u64>(m_stream.commands.size()) *
                                     sizeof(rhi::DrawIndirectCommand)) == 0)
    return;
  const rhi::BufferHandle commands = m_ring.buffer(COMMANDS);

  nx::small_vector<rg::TextureId, MAX_MASK_ATLASES> atlases;
  for (const glm::uvec2 extent : m_stream.atlases)
    atlases.push_back(graph.create({
        .name = "live2d masks",
        .format = rhi::Format::RGBA8_UNORM,
        .width = extent.x,
        .height = extent.y,
        .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled,
    }));

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
        rg::ExecuteFn([this, commands, positions, draws, extent, first,
                       count](rhi::CommandContext &cmd, const rg::Resources &) {
          cmd.set_viewport({.width = nx::cast<f32>(extent.x),
                            .height = nx::cast<f32>(extent.y)});
          cmd.set_scissor({{0, 0}, {extent.x, extent.y}});
          if (count == 0u)
            return;
          cmd.bind_pipeline(m_mask_pipeline);
          PushBlock push;
          push.positions = positions;
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

  // Blend of each model record, in record order, for splitting the multi-draw.
  const u32 total = m_stream.model_count();
  nx::small_vector<r2d::MeshBlend, 256> blends;
  blends.reserve(total);
  for (const ModelDraw &draw : frame.draws)
    if (names_drawable(frame, draw.model, draw.drawable))
      blends.push_back(draw.blend);

  graph.add_pass(
      "live2d.model", rg::SetupFn([target, atlases](rg::Builder &builder) {
        builder.color(0, target);
        for (const rg::TextureId atlas : atlases)
          builder.sample(atlas);
      }),
      rg::ExecuteFn([this, &device, atlases, commands, cameras, positions,
                     draws, blends = std::move(blends),
                     total](rhi::CommandContext &cmd,
                            const rg::Resources &resources) {
        PushBlock push;
        push.cameras = cameras;
        push.positions = positions;
        push.draws = draws;
        for (usize a = 0; a < atlases.size(); ++a)
          push.masks[a] = nx_texture_2d<float4>(pack_texture(
              device.texture_index(resources.texture(atlases[a])), m_sampler));

        // Each run of draws at one blend is one multi-draw, in draw order.
        for (u32 i = 0; i < total;) {
          const r2d::MeshBlend blend = blends[i];
          u32 run = 1;
          while (i + run < total && run < MAX_MULTI_DRAW &&
                 blends[i + run] == blend)
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
