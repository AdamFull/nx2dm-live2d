
#include "live2d/live2d_platform.h"
#include "live2d/live2d_scripting.h"
#include "live2d/live2d_system.h"

#include "app/assets/async_texture_set.h"
#include "app/engine.h"
#include "app/module_system/module.h"
#include "app/rendering/gpu_pipelines.h"
#include "rendering/render2d/frame_packet.h"
#include "scene/scene_json.h"
#if NX_WITH_EDITOR
#include "editor/pick.h"
#endif

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/strings/format.h"

#include <glm/common.hpp>

namespace nxm::live2d {
namespace {

constexpr nx::string_view DRAW_PASS = "live2d.draw";
constexpr nx::string_view TARGETS_PASS = "live2d.targets";
constexpr nx::string_view LOAD_SYSTEM = "live2d.load";
constexpr nx::string_view UPDATE_SYSTEM = "live2d.update";
constexpr nx::string_view EMIT_SYSTEM = "live2d.emit";
constexpr nx::string_view MODULE_SLOT = "live2d";
constexpr nx::string_view WORLD_SLOT = "world";
constexpr nx::string_view SHADER = "live2d/live2d";

constexpr nxe::ModuleService PROVIDED_SERVICES[] = {
    {.id = SERVICE, .version = {1, 0, 0}},
};

/// The models each camera with a render target sees, one frame each.
struct TargetModels {
  nx::vector<Frame> frames;
  nx::vector<u32> cameras;
  usize count = 0;

  void clear() noexcept { count = 0; }

  [[nodiscard]] const Frame *find(const u32 camera) const noexcept {
    for (usize i = 0; i < count; ++i)
      if (cameras[i] == camera)
        return &frames[i];
    return nullptr;
  }
};

/// What @p camera of @p packet sees, on @p layers.
[[nodiscard]] SceneView view_of(nxe::ModuleContext &ctx,
                                const nxe::r2d::FramePacket &packet,
                                const u32 camera, const u32 layers) {
  SceneView view{.camera = camera,
                 .layers = layers,
                 .depth_min = ctx.renderer().depth_min(),
                 .depth_max = ctx.renderer().depth_max(),
                 .materials = ctx.renderer().materials()};
  if (camera < packet.cameras.size()) {
    const GpuCamera2D &seen = packet.cameras[camera];
    view.pixels_per_unit =
        0.5f * glm::abs(glm::vec2(seen.view_proj[0][0] * seen.viewport.x,
                                  seen.view_proj[1][1] * seen.viewport.y));
  }
  return view;
}

class Live2DModule final : public nxe::Module {
public:
  [[nodiscard]] nxe::ModuleDescriptor descriptor() const noexcept override {
    nxe::ModuleDescriptor out{};
    out.id = "live2d";
    out.version = {1, 0, 0};
    out.provided_services = PROVIDED_SERVICES;
    return out;
  }

  bool on_register(nxe::ModuleContext &ctx) override {
    if (!install_platform())
      return false;

    const nxe::rhi::DeviceCaps &caps = ctx.device().caps();
    const u32 max_mask_resolution =
        nx::min(caps.max_texture_2d != 0 ? caps.max_texture_2d : 2048u, 2048u);
    const u64 memory_budget = caps.device_local_memory != 0
                                  ? nx::clamp(caps.device_local_memory / 256u,
                                              u64{4} << 20, u64{32} << 20)
                                  : u64{16} << 20;
    m_system.set_mask_limits(max_mask_resolution, memory_budget);
    m_atlas_limit = max_mask_resolution;
    m_system.set_threads(&ctx.threads());
    if (nx::vfs::AsyncIoService *const io = ctx.async_io())
      m_system.bind_loads(*io, ctx.threads());
    m_system.set_resolver(TextureResolver([this,
                                           &ctx](const nx::string_view path) {
      const nxe::rhi::TextureHandle texture = m_textures.resolve(ctx, path);
      if (!texture.valid())
        return NxTexture2D<float4>::none();
      return NxTexture2D<float4>::from_indices(
          ctx.device().texture_index(texture),
          ctx.samplers().index(nxe::scene::sampler_bilinear()));
    }));

    if (!ctx.service_registrar().provide(SERVICE, PROVIDED_SERVICES[0].version,
                                         m_system)) {
      uninstall_platform();
      return false;
    }
    on_scene_created(ctx, ctx.scene());
#if NX_WITH_EDITOR
    ctx.add_picker(
        {.at = nxe::editor::Picker2D::At(
             [this](const nxe::scene::Scene &scene, const glm::vec2 point,
                    nx::vector<nxe::editor::PickHit> &out) {
               m_system.pick(scene.registry(), point,
                             [&](const nxe::scene::Entity e, const i32 layer) {
                               out.push_back({e, layer});
                             });
             }),
         .within = nxe::editor::Picker2D::Within(
             [this](const nxe::scene::Scene &scene, const glm::vec2 lo,
                    const glm::vec2 hi, nx::vector<nxe::scene::Entity> &out) {
               m_system.pick_within(
                   scene.registry(), lo, hi,
                   [&](const nxe::scene::Entity e, i32) { out.push_back(e); });
             })});
#endif
    return true;
  }

  void on_expose_scripts(nxe::script::Host &host,
                         nxe::ModuleContext &ctx) override {
    expose_live2d_services(host, ctx);
  }

  void on_scene_created(nxe::ModuleContext &,
                        nxe::scene::Scene &scene) override {
    Live2DSystem::register_components(scene.registry());
    scene.formats().add(
        nxe::scene::described<Live2DModel>("live2d", "live2d_models"));
  }

  void on_scene_destroyed(nxe::ModuleContext &,
                          nxe::scene::Scene &scene) override {
    m_system.forget(scene.registry());
    for (usize i = 0; i < m_refreshed.size(); ++i)
      if (m_refreshed[i].registry == &scene.registry()) {
        m_refreshed.erase(m_refreshed.begin() + nx::cast<isize>(i));
        break;
      }
  }

  void on_hot_reload(nxe::ModuleContext &ctx) override {
    ctx.for_each_scene([this](nxe::scene::Scene &scene) {
      (void)m_system.reload_changed(scene.registry());
    });
    if (!ctx.shader_reloaded(SHADER))
      return;
    reset_pipelines(ctx);
  }

  bool on_attach(nxe::ModuleContext &ctx) override {
    m_sampler = ctx.samplers().index(nxe::scene::sampler_bilinear());

    ctx.schedule().define(
        LOAD_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
          refresh_textures(ctx, ctx.scene().registry());
          const usize loaded =
              m_system.load_pending(ctx.scene().registry(), c.dt);
          release_unused_textures(ctx, loaded != 0);
        }));
    ctx.schedule().add(nxe::sys::Stage::Update, LOAD_SYSTEM);
    // Other scenes load and pose alone; the lip sync is the game's.
    ctx.add_scene_update(nxe::SceneUpdateFn(
        [this, &ctx](nxe::scene::Scene &scene, const f32 dt) {
          refresh_textures(ctx, scene.registry());
          (void)m_system.load_pending(scene.registry(), dt);
          (void)m_system.update(scene.registry(), scene.assets(), dt);
        }));

    ctx.schedule().define(
        UPDATE_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
          (void)drive_lip_sync(ctx, m_system);
          (void)m_system.update(ctx.scene().registry(), ctx.scene().assets(),
                                c.dt);
        }));
    ctx.schedule().add(nxe::sys::Stage::Update, UPDATE_SYSTEM);

    ctx.schedule().define(
        EMIT_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &) {
          nxe::r2d::FramePacket *const packet = ctx.frame_packet();
          if (packet == nullptr)
            return;
          Frame &frame = packet->channel<Frame>();
          frame.clear();
          (void)m_system.emit(ctx.scene().registry(), frame,
                              view_of(ctx, *packet, packet->active_camera,
                                      nxe::scene::WINDOW_LAYER));
          // Each camera drawing into a texture sees its own layers.
          TargetModels &targets = packet->channel<TargetModels>();
          targets.clear();
          for (const nxe::r2d::TargetView &target : packet->targets) {
            if (targets.count == nxe::MAX_RENDER_TARGETS)
              break;
            if (targets.count == targets.frames.size()) {
              targets.frames.emplace_back();
              targets.cameras.push_back(0);
            }
            Frame &seen = targets.frames[targets.count];
            seen.clear();
            targets.cameras[targets.count] = target.camera;
            ++targets.count;
            (void)m_system.emit(
                ctx.scene().registry(), seen,
                view_of(ctx, *packet, target.camera, target.layers));
          }
        }));
    ctx.schedule().add(nxe::sys::Stage::Present, EMIT_SYSTEM);
    // A layer bound to a scene draws its models through the layer's camera.
    ctx.add_layer_extract(nxe::LayerExtractFn(
        [this, &ctx](nxe::scene::Scene &scene, nxe::r2d::FramePacket &layer) {
          Frame &frame = layer.channel<Frame>();
          frame.clear();
          (void)m_system.emit(scene.registry(), frame,
                              view_of(ctx, layer, layer.active_camera,
                                      nxe::scene::WINDOW_LAYER));
        }));
    ctx.schedule()
        .declare<const Live2DModel, Live2DRuntime,
                 const nxe::scene::WorldTransform2D,
                 const nxe::scene::RenderLayers>(EMIT_SYSTEM);

    ctx.passes().define(
        DRAW_PASS,
        nxe::PassFn([this, &ctx](nxe::rg::RenderGraph &graph,
                                 nxe::RenderContext &context) {
          const Frame *const frame = context.packet != nullptr
                                         ? context.packet->find_channel<Frame>()
                                         : nullptr;
          if (frame == nullptr || frame->empty())
            return;

          const nxe::rg::TextureId target =
              context.target(nxe::TARGET_SCENE_COLOR);
          const nxe::rhi::Format format = graph.format(target);
          ModelRenderer *const renderer =
              world_renderer(ctx, context.node, context.window);
          if (renderer == nullptr)
            return;
          use_pipelines(ctx, *renderer, format);
          renderer->draw(ctx.device(), graph, target, format,
                         context.scene_push.cameras.value, *frame);
        }),
        nxe::PassTraits{.scope = nxe::PassScope::Node,
                        .needs = nxe::PassNeed::View});

    static constexpr nx::string_view MINE[] = {DRAW_PASS};
    const nx::string_view slot =
        ctx.has_pass_slot(MODULE_SLOT) ? MODULE_SLOT : WORLD_SLOT;
    if (!ctx.fill_pass_slot(slot, MINE, name()))
      nx::logw("live2d: nothing to fill; the frame has no '{}' slot", slot);
    ctx.add_preview_passes(MINE);

    ctx.passes().define(
        TARGETS_PASS,
        nxe::PassFn([this, &ctx](nxe::rg::RenderGraph &graph,
                                 nxe::RenderContext &context) {
          const TargetModels *const targets =
              context.packet != nullptr
                  ? context.packet->find_channel<TargetModels>()
                  : nullptr;
          if (targets == nullptr)
            return;
          const Frame *const frame = targets->find(context.view.camera);
          if (frame == nullptr || frame->empty())
            return;
          const nxe::rg::TextureId target =
              context.target(nxe::TARGET_BACKBUFFER);
          const nxe::rhi::Format format = graph.format(target);
          ModelRenderer *const renderer =
              target_renderer(ctx, context.view.target);
          if (renderer == nullptr)
            return;
          use_pipelines(ctx, *renderer, format);
          renderer->draw(ctx.device(), graph, target, format,
                         context.scene_push.cameras.value, *frame);
        }),
        nxe::PassTraits{.scope = nxe::PassScope::Node,
                        .needs = nxe::PassNeed::View});
    // A frame without the slot draws no targets, which is no fault.
    static constexpr nx::string_view TARGETS[] = {TARGETS_PASS};
    if (ctx.has_pass_slot(nxe::TARGETS_SLOT) &&
        !ctx.fill_pass_slot(nxe::TARGETS_SLOT, TARGETS, name()))
      nx::logw("live2d: the '{}' slot is filled already", nxe::TARGETS_SLOT);

    nx::logi("live2d: attached");
    return true;
  }

  void on_detach(nxe::ModuleContext &ctx) override {
    reset_pipelines(ctx);
    m_world_renderers.for_each(
        [](ModelRenderer &renderer) { renderer.shutdown(); });
    m_world_renderers.clear();
    m_target_renderers.for_each(
        [](ModelRenderer &renderer) { renderer.shutdown(); });
    m_target_renderers.clear();
    m_sampler = 0;
  }

  void on_unregister(nxe::ModuleContext &ctx) override {
    m_system.set_resolver({});
    m_textures.release_all(ctx);
    m_system.shutdown_loads();
    m_system.release_mocs();
    uninstall_platform();
  }

  void on_low_memory(nxe::ModuleContext &ctx) override {
    const usize reduced = m_system.on_low_memory(ctx.scene().registry());
    nx::logi("live2d: low memory reduced {} mask layout(s); future masks are "
             "capped at {}px and {} KiB per frame",
             reduced, m_system.mask_resolution_limit(),
             m_system.mask_budget() >> 10);
  }

private:
  void reset_pipelines(nxe::ModuleContext &ctx) {
    const nxe::rhi::PipelineHandle
        empty[nx::cast<usize>(nxe::r2d::MeshBlend::Count)] = {};
    m_world_renderers.for_each([&](ModelRenderer &renderer) {
      renderer.set_pipelines({}, empty, nxe::rhi::Format::Unknown);
    });
    m_target_renderers.for_each([&](ModelRenderer &renderer) {
      renderer.set_pipelines({}, empty, nxe::rhi::Format::Unknown);
    });
    if (m_mask_request.valid())
      (void)ctx.release_pipeline_load(m_mask_request);
    m_mask_request = {};
    for (nxe::FormatPipelines &blend : m_model_pipelines)
      blend.release([&ctx](const nxe::PipelineLoadRequest request) {
        (void)ctx.release_pipeline_load(request);
      });
  }

  /// Gives @p renderer the pipelines for a target of @p format once they have
  /// all compiled; until then it keeps what it had, and draws nothing into a
  /// target of another format.
  void use_pipelines(nxe::ModuleContext &ctx, ModelRenderer &renderer,
                     const nxe::rhi::Format format) {
    if (!m_mask_request.valid()) {
      nxe::rhi::GraphicsPipelineDesc desc;
      desc.name = "live2d mask";
      desc.vertex.entry_point = "mask_vs";
      desc.fragment.entry_point = "mask_fs";
      desc.color_formats[0] = nxe::rhi::Format::RGBA8_UNORM;
      desc.color_count = 1;
      desc.blend[0] = {.enabled = true,
                       .mode = nxe::rhi::BlendMode::PremultipliedAdditive};
      m_mask_request = ctx.load_graphics_pipeline_async(SHADER, desc);
    }
    const nxe::rhi::PipelineHandle mask = ctx.loaded_pipeline(m_mask_request);
    nxe::rhi::PipelineHandle
        models[nx::cast<usize>(nxe::r2d::MeshBlend::Count)] = {};
    bool compiled = mask.valid();
    for (usize i = 0; i < nx::array_size(models); ++i) {
      const nxe::PipelineLoadRequest request = m_model_pipelines[i].request(
          format, 1u, [&](const nxe::rhi::Format color, u32) {
            nxe::rhi::GraphicsPipelineDesc desc;
            desc.name = "live2d model";
            desc.vertex.entry_point = "model_vs";
            desc.fragment.entry_point = "model_fs";
            desc.color_formats[0] = color;
            desc.color_count = 1;
            desc.blend[0] = {
                .enabled = true,
                .mode = pipeline_blend(nx::cast<nxe::r2d::MeshBlend>(i))};
            return ctx.load_graphics_pipeline_async(SHADER, desc);
          });
      models[i] = ctx.loaded_pipeline(request);
      compiled = compiled && models[i].valid();
    }
    if (compiled)
      renderer.set_pipelines(mask, models, format);
  }

  [[nodiscard]] nx::unique_ptr<ModelRenderer>
  make_renderer(nxe::ModuleContext &ctx, const nx::string_view name) {
    nx::unique_ptr<ModelRenderer> renderer = nx::make_unique<ModelRenderer>();
    if (!renderer->init(ctx.device(), m_sampler)) {
      if (!m_ring_warned)
        nx::logw("live2d: no upload ring for '{}'; models will not draw", name);
      m_ring_warned = true;
      return nullptr;
    }
    renderer->set_name(name);
    renderer->set_atlas_limit(m_atlas_limit);
    return renderer;
  }

  [[nodiscard]] ModelRenderer *world_renderer(nxe::ModuleContext &ctx,
                                              const nxe::ComposedNode *node,
                                              const nxe::rt::WindowId window) {
    // One draws once a frame: windows that share a node name, as previews
    // all do, each have their own.
    nx::string name(node != nullptr ? node->name.view() : nx::string_view{});
    if (window != nxe::rt::PRIMARY_WINDOW)
      name = nx::format("{}#{}", name, window.raw());
    ModelRenderer *const renderer = m_world_renderers.renderer(
        name.view(), [&](const nx::string_view renderer_name) {
          return make_renderer(ctx, renderer_name);
        });
    if (renderer == nullptr &&
        m_world_renderers.size() == NodeRenderers::MAX_NODES &&
        !m_nodes_warned) {
      nx::logw("live2d: models draw in at most {} nodes; '{}' is not one",
               NodeRenderers::MAX_NODES, name);
      m_nodes_warned = true;
    }
    return renderer;
  }

  /// The renderer for the camera target @p path: each its own, since one
  /// draws once a frame into one target.
  [[nodiscard]] ModelRenderer *target_renderer(nxe::ModuleContext &ctx,
                                               const nx::string_view path) {
    return m_target_renderers.renderer(
        path, [&](const nx::string_view renderer_name) {
          return make_renderer(ctx, renderer_name);
        });
  }

  NodeRenderers m_world_renderers;
  bool m_nodes_warned = false;
  bool m_ring_warned = false;
  /// One for each camera target drawn, by its path.
  NodeRenderers m_target_renderers;
  u32 m_atlas_limit = DEFAULT_ATLAS_LIMIT;
  Live2DSystem m_system;
  /// Lets go of texture pages no model uses any more, once a model has come
  /// or gone: a companion switched out must not keep its pages resident.
  void release_unused_textures(nxe::ModuleContext &ctx, const bool loaded) {
    m_in_use.clear();
    usize models = 0;
    ctx.for_each_scene([&](nxe::scene::Scene &scene) {
      models += m_system.textures_in_use(scene.registry(), m_in_use);
    });
    if (!loaded && models == m_swept_models)
      return;
    m_swept_models = models;
    const usize released =
        m_textures.release_unused(ctx, [this](const nx::string_view path) {
          for (const nx::string_view used : m_in_use)
            if (used == path)
              return true;
          return false;
        });
    if (released != 0)
      nx::logd("live2d: released {} texture pages no model uses", released);
  }

  nxe::AsyncTextureSet m_textures;
  nx::vector<nx::string_view> m_in_use;
  usize m_swept_models = 0;

  /// Points @p registry's models at the textures that arrived since it last
  /// looked, whichever scene's update saw them arrive.
  void refresh_textures(nxe::ModuleContext &ctx,
                        nxe::scene::registry_t &registry) {
    if (m_textures.pump(ctx) != 0)
      ++m_texture_arrivals;
    Refreshed *seen = nullptr;
    for (Refreshed &held : m_refreshed)
      if (held.registry == &registry)
        seen = &held;
    if (seen == nullptr) {
      m_refreshed.push_back({&registry, 0});
      seen = &m_refreshed.back();
    }
    if (seen->arrivals == m_texture_arrivals)
      return;
    seen->arrivals = m_texture_arrivals;
    (void)m_system.refresh_textures(registry);
  }

  struct Refreshed {
    const nxe::scene::registry_t *registry = nullptr;
    u64 arrivals = 0;
  };
  /// Counts the pumps that brought textures.
  u64 m_texture_arrivals = 0;
  nx::vector<Refreshed> m_refreshed;
  nxe::PipelineLoadRequest m_mask_request;
  /// By blend, for each format a model has drawn into.
  nxe::FormatPipelines
      m_model_pipelines[nx::cast<usize>(nxe::r2d::MeshBlend::Count)];
  u32 m_sampler = 0;
};

} // namespace
} // namespace nxm::live2d

NX_DECLARE_MODULE(live2d, nxm::live2d::Live2DModule)
