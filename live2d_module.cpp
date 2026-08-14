/**
 * @file live2d_module.cpp
 * @brief What tells an Engine about Live2D, and the only file here that knows
 * an Engine exists.
 */

#include "live2d/live2d_platform.h"
#include "live2d/live2d_scripting.h"
#include "live2d/live2d_system.h"

#include "core/app/engine.h"
#include "core/app/module.h"
#include "core/scene/scene_json.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::live2d {
namespace {

constexpr nx::string_view DRAW_PASS = "live2d.draw";
constexpr nx::string_view LOAD_SYSTEM = "live2d.load";
constexpr nx::string_view UPDATE_SYSTEM = "live2d.update";
constexpr nx::string_view EMIT_SYSTEM = "live2d.emit";
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
    m_system.set_resolver(TextureResolver([&ctx](const nx::string_view path) {
      const nxe::rhi::TextureHandle texture = ctx.load_texture(path);
      if (!texture.valid())
        return pack_texture(NX_TEXTURE_NONE, 0);
      return pack_texture(ctx.device().texture_index(texture),
                          ctx.samplers().index(nxe::scene::sampler_bilinear()));
    }));

    if (!ctx.services().provide(SERVICE, PROVIDED_SERVICES[0].version,
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

  bool on_attach(nxe::ModuleContext &ctx) override {
    const bool can_draw =
        m_renderer.init(ctx.device(), ctx.load_shader(SHADER),
                        ctx.samplers().index(nxe::scene::sampler_bilinear()));
    if (!can_draw)
      nx::logw("live2d: no renderer; models will load and pose but not draw");

    ctx.schedule().define(
        LOAD_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
          (void)m_system.load_pending(ctx.scene().registry(), c.dt);
        }));
    ctx.schedule().add(nxe::sys::Stage::Update, LOAD_SYSTEM);

    ctx.schedule().define(
        UPDATE_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
          (void)drive_lip_sync(ctx, m_system);
          (void)m_system.update(ctx.scene().registry(), c.dt);
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
                               .depth_max = ctx.renderer().depth_max()};
          (void)m_system.emit(ctx.scene().registry(), frame, view);
        }));
    ctx.schedule().add(nxe::sys::Stage::Present, EMIT_SYSTEM);

    if (!can_draw)
      return true;

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
          m_renderer.draw(ctx.device(), graph,
                          context.target(nxe::TARGET_SCENE_COLOR), format,
                          context.scene_push.cameras, *frame);
        }));

    static constexpr nx::string_view MINE[] = {DRAW_PASS};
    if (!ctx.fill_pass_slot(WORLD_SLOT, MINE, name()))
      nx::logw("live2d: nothing to fill; the frame has no '{}' slot",
               WORLD_SLOT);

    nx::logi("live2d: attached");
    return true;
  }

  void on_detach(nxe::ModuleContext &ctx) override {
    m_renderer.shutdown(ctx.device());
  }

  void on_unregister(nxe::ModuleContext &) override {
    m_system.set_resolver({});
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
  ModelRenderer m_renderer;
  Live2DSystem m_system;
};

} // namespace
} // namespace nxm::live2d

NX_DECLARE_MODULE(live2d, nxm::live2d::Live2DModule)
