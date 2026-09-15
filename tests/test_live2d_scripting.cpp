
#include "framework/nxtest.h"

#include "app/engine.h"
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

  [[nodiscard]] const script::Host::ServiceInfo *
  find(const nx::string_view name) const {
    for (const script::Host::ServiceInfo &one : services)
      if (one.name == name)
        return &one;
    return nullptr;
  }
};

} // namespace

TEST_CASE("live2d scripting: every service is exposed with the shape a script "
          "is told about") {
  const Exposed exposed;

  static constexpr struct {
    nx::string_view name;
    nx::string_view signature;
  } WANT[] = {
      {"live2d_play", "(number,string,number,boolean)->(boolean)"},
      {"live2d_expression", "(number,string)->(boolean)"},
      {"live2d_finished", "(number)->(boolean)"},
      {"live2d_set_param", "(number,string,number)->(boolean)"},
      {"live2d_param", "(number,string)->(number)"},
      {"live2d_visible", "(number,boolean)->(boolean)"},
      {"live2d_mouth", "(number,number)->(boolean)"},
      {"live2d_speak", "(number,string)->(boolean)"},
  };

  CHECK(exposed.services.size() == nx::array_size(WANT));
  for (const auto &want : WANT) {
    const script::Host::ServiceInfo *const found = exposed.find(want.name);
    REQUIRE(found != nullptr);
    CHECK(found->signature == want.signature);
  }
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
