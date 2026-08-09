/**
 * @file live2d_module.cpp
 * @brief What tells an Engine about Live2D, and the only file here that knows
 * an Engine exists.
 */

#include "live2d/live2d_pass.h"
#include "live2d/live2d_platform.h"

#include "core/app/engine.h"
#include "core/app/module.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::live2d {
namespace {

constexpr nx::string_view DRAW_PASS = "live2d.draw";
constexpr nx::string_view EMIT_SYSTEM = "live2d.emit";
constexpr nx::string_view WORLD_SLOT = "world";
constexpr nx::string_view SHADER = "live2d/live2d";

class Live2DModule final : public nxe::Module {
public:
  [[nodiscard]] nx::string_view name() const noexcept override {
    return "live2d";
  }

  bool on_register(nxe::Engine &) override { return install_platform(); }

  bool on_attach(nxe::Engine &engine) override {
    if (!m_renderer.init(
            engine.device(), engine.load_shader(SHADER),
            engine.samplers().index(nxe::scene::sampler_bilinear()))) {
      // Not a refusal. A build whose shaders were not compiled should still
      // start; what it loses is Live2D drawing, which the log already said.
      nx::logw("live2d: attached without a renderer; no model will draw");
      return true;
    }

    engine.schedule().define(
        EMIT_SYSTEM, nxe::sys::SystemFn([&engine](const nxe::sys::Context &) {
          nxe::r2d::FramePacket *const packet = engine.frame_packet();
          if (packet != nullptr)
            packet->channel<Frame>().clear();
        }));
    engine.schedule().add(nxe::sys::Stage::Present, EMIT_SYSTEM);

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

  void on_low_memory(nxe::Engine &) override {
    nx::logi("live2d: nothing held that can be dropped");
  }

private:
  ModelRenderer m_renderer;
};

} // namespace
} // namespace nxm::live2d

NX_DECLARE_MODULE(live2d, nxm::live2d::Live2DModule)
