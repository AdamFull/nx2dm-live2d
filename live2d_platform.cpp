
#include "live2d/live2d_platform.h"

#include "core/foundation/diagnostics/log.h"

#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>
#include <Live2DCubismCore.hpp>

namespace nxm::live2d {
namespace {

namespace csm = Live2D::Cubism::Framework;
namespace core = Live2D::Cubism::Core;

class Allocator final : public csm::ICubismAllocator {
public:
  void *Allocate(const csm::csmSizeType size) override {
    return nx::mem_alloc(size);
  }
  void Deallocate(void *memory) override { nx::mem_free(memory); }

  void *AllocateAligned(const csm::csmSizeType size,
                        const csm::csmUint32 alignment) override {
    return nx::mem_alloc(size, alignment);
  }
  void DeallocateAligned(void *memory) override { nx::mem_free(memory); }
};

void log_line(const char *const message) {
  nx::logi("live2d: {}", message != nullptr ? message : "");
}

Allocator &allocator() {
  static Allocator instance;
  return instance;
}

bool g_started = false;

}

CoreVersion core_version() noexcept {
  const u32 packed = nx::cast<u32>(core::csmGetVersion());
  return {(packed & 0xFF00'0000u) >> 24, (packed & 0x00FF'0000u) >> 16,
          packed & 0x0000'FFFFu};
}

u32 latest_moc_version() noexcept {
  return nx::cast<u32>(core::csmGetLatestMocVersion());
}

bool install_platform() {
  if (g_started)
    return true;

  static csm::CubismFramework::Option option{};
  option.LogFunction = log_line;
  option.LoggingLevel = csm::CubismFramework::Option::LogLevel_Warning;

  if (!csm::CubismFramework::StartUp(&allocator(), &option)) {
    nx::loge("live2d: the Cubism framework would not start");
    return false;
  }
  csm::CubismFramework::Initialize();
  g_started = true;

  const CoreVersion version = core_version();
  nx::logi("live2d: Cubism Core {}.{}.{}, moc3 up to format {}", version.major,
           version.minor, version.patch, latest_moc_version());
  return true;
}

void uninstall_platform() {
  if (!g_started)
    return;
  csm::CubismFramework::Dispose();
  csm::CubismFramework::CleanUp();
  g_started = false;
}

}
