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

class Live2DModule final : public nxe::Module {
public:
  [[nodiscard]] nx::string_view name() const noexcept override {
    return "live2d";
  }

  bool on_register(nxe::Engine &engine) override {
    if (!install_platform())
      return false;
    Live2DSystem::register_components(engine.scene().registry());
    engine.scene().formats().add(
        nxe::scene::described<Live2DModel>("live2d", "live2d_models"));
    return true;
  }

  void on_expose_scripts(nxe::script::Host &host,
                         nxe::Engine &engine) override {
    expose_live2d_services(host, engine);
  }

  bool on_attach(nxe::Engine &engine) override {
    const bool can_draw = m_renderer.init(
        engine.device(), engine.load_shader(SHADER),
        engine.samplers().index(nxe::scene::sampler_bilinear()));
    if (!can_draw)
      nx::logw("live2d: no renderer; models will load and pose but not draw");

    const nxe::rhi::DeviceCaps &caps = engine.device().caps();
    const u32 max_mask_resolution =
        nx::min(caps.max_texture_2d != 0 ? caps.max_texture_2d : 2048u,
                2048u);
    const u64 memory_budget =
        caps.device_local_memory != 0
            ? nx::clamp(caps.device_local_memory / 256u, u64{4} << 20,
                        u64{32} << 20)
            : u64{16} << 20;
    m_system.set_mask_limits(max_mask_resolution, memory_budget);

    m_system.set_resolver(
        TextureResolver([&engine](const nx::string_view path) {
          const nxe::rhi::TextureHandle texture = engine.load_texture(path);
          if (!texture.valid())
            return pack_texture(NX_TEXTURE_NONE, 0);
          return pack_texture(
              engine.device().texture_index(texture),
              engine.samplers().index(nxe::scene::sampler_bilinear()));
        }));

    engine.schedule().define(
        LOAD_SYSTEM,
        nxe::sys::SystemFn([this, &engine](const nxe::sys::Context &c) {
          (void)m_system.load_pending(engine.scene().registry(), c.dt);
        }));
    engine.schedule().add(nxe::sys::Stage::Update, LOAD_SYSTEM);

    engine.schedule().define(
        UPDATE_SYSTEM,
        nxe::sys::SystemFn([this, &engine](const nxe::sys::Context &c) {
          (void)drive_lip_sync(engine, m_system);
          (void)m_system.update(engine.scene().registry(), c.dt);
        }));
    engine.schedule().add(nxe::sys::Stage::Update, UPDATE_SYSTEM);

    engine.schedule().define(
        EMIT_SYSTEM,
        nxe::sys::SystemFn([this, &engine](const nxe::sys::Context &) {
          nxe::r2d::FramePacket *const packet = engine.frame_packet();
          if (packet == nullptr)
            return;
          Frame &frame = packet->channel<Frame>();
          frame.clear();
          const SceneView view{.camera = packet->active_camera,
                               .depth_min = engine.renderer().depth_min(),
                               .depth_max = engine.renderer().depth_max()};
          (void)m_system.emit(engine.scene().registry(), frame, view);
        }));
    engine.schedule().add(nxe::sys::Stage::Present, EMIT_SYSTEM);

    if (!can_draw)
      return true;

    engine.passes().define(
        DRAW_PASS, nxe::PassFn([this, &engine](nxe::rg::RenderGraph &graph,
                                               nxe::RenderContext &context) {
          const Frame *const frame = context.packet != nullptr
                                         ? context.packet->find_channel<Frame>()
                                         : nullptr;
          if (frame == nullptr || frame->empty())
            return;

          const nxe::rhi::Format format =
              engine.config().scene_format == nxe::rhi::Format::Unknown
                  ? engine.device().swapchain_format()
                  : engine.config().scene_format;
          m_renderer.draw(engine.device(), graph,
                          context.target(nxe::TARGET_SCENE_COLOR), format,
                          context.scene_push.cameras, *frame);
        }));

    static constexpr nx::string_view MINE[] = {DRAW_PASS};
    if (!engine.fill_pass_slot(WORLD_SLOT, MINE, name()))
      nx::logw("live2d: nothing to fill; the frame has no '{}' slot",
               WORLD_SLOT);

    nx::logi("live2d: attached");
    return true;
  }

  void on_detach(nxe::Engine &engine) override {
    m_renderer.shutdown(engine.device());
    uninstall_platform();
  }

  void on_low_memory(nxe::Engine &engine) override {
    const usize reduced =
        m_system.on_low_memory(engine.scene().registry());
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
