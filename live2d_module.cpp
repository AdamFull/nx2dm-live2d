
#include "live2d/live2d_platform.h"
#include "live2d/live2d_scripting.h"
#include "live2d/live2d_system.h"

#include "app/assets/async_texture_set.h"
#include "app/engine.h"
#include "app/module_system/module.h"
#include "scene/scene_json.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::live2d {
namespace {

constexpr nx::string_view DRAW_PASS = "live2d.draw";
constexpr nx::string_view LOAD_SYSTEM = "live2d.load";
constexpr nx::string_view UPDATE_SYSTEM = "live2d.update";
constexpr nx::string_view EMIT_SYSTEM = "live2d.emit";
constexpr nx::string_view MODULE_SLOT = "live2d";
constexpr nx::string_view WORLD_SLOT = "world";
constexpr nx::string_view SHADER = "live2d/live2d";
constexpr nxe::ModuleService PROVIDED_SERVICES[] = {
    {.id = SERVICE, .version = {1, 0, 0}},
};

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
    m_system.set_threads(&ctx.threads());
    m_system.set_resolver(TextureResolver([this,
                                           &ctx](const nx::string_view path) {
      const nxe::rhi::TextureHandle texture = m_textures.resolve(ctx, path);
      if (!texture.valid())
        return pack_texture(NX_TEXTURE_NONE, 0);
      return pack_texture(ctx.device().texture_index(texture),
                          ctx.samplers().index(nxe::scene::sampler_bilinear()));
    }));

    if (!ctx.service_registrar().provide(SERVICE, PROVIDED_SERVICES[0].version,
                                         m_system)) {
      uninstall_platform();
      return false;
    }
    Live2DSystem::register_components(ctx.scene().registry());
    ctx.scene().formats().add(
        nxe::scene::described<Live2DModel>("live2d", "live2d_models"));
    return true;
  }

  void on_expose_scripts(nxe::script::Host &host,
                         nxe::ModuleContext &ctx) override {
    expose_live2d_services(host, ctx);
  }

  void on_hot_reload(nxe::ModuleContext &ctx) override {
    (void)m_system.reload_changed(ctx.scene().registry());
    if (!ctx.shader_reloaded(SHADER))
      return;
    reset_pipelines(ctx);
  }

  bool on_attach(nxe::ModuleContext &ctx) override {
    m_sampler = ctx.samplers().index(nxe::scene::sampler_bilinear());
    if (!m_renderer.init(ctx.device(), m_sampler))
      nx::logw("live2d: no upload ring; models will not draw");

    ctx.schedule().define(
        LOAD_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
          if (m_textures.pump(ctx) != 0)
            (void)m_system.reload_changed(ctx.scene().registry(), true);
          (void)m_system.load_pending(ctx.scene().registry(), c.dt);
        }));
    ctx.schedule().add(nxe::sys::Stage::Update, LOAD_SYSTEM);

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
          const SceneView view{.camera = packet->active_camera,
                               .depth_min = ctx.renderer().depth_min(),
                               .depth_max = ctx.renderer().depth_max(),
                               .materials = ctx.renderer().materials()};
          (void)m_system.emit(ctx.scene().registry(), frame, view);
        }));
    ctx.schedule().add(nxe::sys::Stage::Present, EMIT_SYSTEM);

    ctx.passes().define(
        DRAW_PASS, nxe::PassFn([this, &ctx](nxe::rg::RenderGraph &graph,
                                            nxe::RenderContext &context) {
          const Frame *const frame = context.packet != nullptr
                                         ? context.packet->find_channel<Frame>()
                                         : nullptr;
          if (frame == nullptr || frame->empty())
            return;

          const nxe::rhi::Format format =
              ctx.config().scene_format == nxe::rhi::Format::Unknown
                  ? ctx.device().swapchain_format()
                  : ctx.config().scene_format;
          ensure_pipelines(ctx, format);
          m_renderer.draw(ctx.device(), graph,
                          context.target(nxe::TARGET_SCENE_COLOR), format,
                          context.scene_push.cameras, *frame);
        }));

    static constexpr nx::string_view MINE[] = {DRAW_PASS};
    const nx::string_view slot =
        ctx.has_pass_slot(MODULE_SLOT) ? MODULE_SLOT : WORLD_SLOT;
    if (!ctx.fill_pass_slot(slot, MINE, name()))
      nx::logw("live2d: nothing to fill; the frame has no '{}' slot", slot);

    nx::logi("live2d: attached");
    return true;
  }

  void on_detach(nxe::ModuleContext &ctx) override {
    reset_pipelines(ctx);
    m_renderer.shutdown();
    m_sampler = 0;
  }

  void on_unregister(nxe::ModuleContext &ctx) override {
    m_system.set_resolver({});
    m_textures.release_all(ctx);
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
    m_renderer.set_pipelines({}, empty, nxe::rhi::Format::Unknown);
    if (m_mask_request.valid())
      (void)ctx.release_pipeline_load(m_mask_request);
    m_mask_request = {};
    for (nxe::PipelineLoadRequest &request : m_model_requests) {
      if (request.valid())
        (void)ctx.release_pipeline_load(request);
      request = {};
    }
    m_pipeline_format = nxe::rhi::Format::Unknown;
  }

  void ensure_pipelines(nxe::ModuleContext &ctx,
                        const nxe::rhi::Format format) {
    if (m_pipeline_format != format)
      reset_pipelines(ctx);
    m_pipeline_format = format;
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
    for (usize i = 0; i < nx::array_size(m_model_requests); ++i) {
      if (m_model_requests[i].valid())
        continue;
      nxe::rhi::GraphicsPipelineDesc desc;
      desc.name = "live2d model";
      desc.vertex.entry_point = "model_vs";
      desc.fragment.entry_point = "model_fs";
      desc.color_formats[0] = format;
      desc.color_count = 1;
      desc.blend[0] = {.enabled = true,
                       .mode =
                           pipeline_blend(nx::cast<nxe::r2d::MeshBlend>(i))};
      m_model_requests[i] = ctx.load_graphics_pipeline_async(SHADER, desc);
    }
    const nxe::rhi::PipelineHandle mask = ctx.loaded_pipeline(m_mask_request);
    nxe::rhi::PipelineHandle
        models[nx::cast<usize>(nxe::r2d::MeshBlend::Count)] = {};
    if (!mask.valid())
      return;
    for (usize i = 0; i < nx::array_size(models); ++i) {
      models[i] = ctx.loaded_pipeline(m_model_requests[i]);
      if (!models[i].valid())
        return;
    }
    m_renderer.set_pipelines(mask, models, format);
  }

  ModelRenderer m_renderer;
  Live2DSystem m_system;
  nxe::AsyncTextureSet m_textures;
  nxe::PipelineLoadRequest m_mask_request;
  nxe::PipelineLoadRequest
      m_model_requests[nx::cast<usize>(nxe::r2d::MeshBlend::Count)] = {};
  nxe::rhi::Format m_pipeline_format = nxe::rhi::Format::Unknown;
  u32 m_sampler = 0;
};

} // namespace
} // namespace nxm::live2d

NX_DECLARE_MODULE(live2d, nxm::live2d::Live2DModule)
