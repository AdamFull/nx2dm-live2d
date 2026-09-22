
#include "framework/nxtest.h"

#include "app/engine.h"
#include "core/foundation/platform/filesystem.h"
#include "script/luau/luau_bindings.h"
#include "script/script_host.h"
#include "live2d/live2d_scripting.h"

namespace {

using namespace nxm::live2d;
namespace script = nxe::script;

struct Exposed {
  nxe::Engine engine{nxe::Game{}};
  nxe::ModuleContext ctx{engine};
  script::Host host;
  nx::vector<script::Host::ServiceInfo> services;

  Exposed() {
    expose_live2d_services(host, ctx);
    services = host.services();
  }
};

} // namespace

TEST_CASE("live2d scripting: every service is exposed as script-services.json "
          "declares it") {
  const Exposed exposed;
  const auto manifest = nx::fs::file_read_text(
      nx::fs::path_view(NX_MODULE_SERVICES_MANIFEST));
  REQUIRE(manifest);

  nx::string error;
  if (!script::luau_manifest_agrees(manifest.value(), exposed.services, error))
    FAIL(error.c_str());
}

TEST_CASE("live2d scripting: the module hands them over on its own") {
  std::unique_ptr<nxe::Module> found;
  for (const nxe::ModuleFactory factory : nxe::enabled_module_factories()) {
    std::unique_ptr<nxe::Module> module = factory();
    if (module != nullptr && module->name() == "live2d")
      found = std::move(module);
  }
  REQUIRE(found != nullptr);

  nxe::Engine engine{nxe::Game{}};
  nxe::ModuleContext ctx{engine};
  script::Host host;
  found->on_expose_scripts(host, ctx);

  script::Host direct;
  expose_live2d_services(direct, ctx);
  CHECK(host.exposed_count() == direct.exposed_count());
  CHECK(host.exposed_count() > 0u);
}
