/**
 * @file test_live2d_scripting.cpp
 * @brief What the module hands a script, and the shape of it.
 *
 * The build writes these signatures into the declarations a type checker reads,
 * so a renamed service or a moved argument is not a compile error anywhere - it
 * is a script that stops type-checking, or worse, one that still does and calls
 * the wrong thing. This is where that gets caught.
 *
 * No backend and no started Engine: Host::expose records, and only bind() needs
 * a VM. It is the same trick tools/make_host_declarations plays.
 */

#include "framework/nxtest.h"

#include "core/app/engine.h"
#include "core/script/script_host.h"
#include "live2d/live2d_scripting.h"

namespace {

using namespace nxm::live2d;
namespace script = nxe::script;

struct Exposed {
  nxe::Engine engine{nullptr};
  script::Host host;
  nx::vector<script::Host::ServiceInfo> services;

  Exposed() {
    expose_live2d_services(host, engine);
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

  // Spelled out rather than counted: the whole point is that a script's
  // declaration and this list cannot drift, and a count would let a rename
  // through.
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
  // Through Module::on_expose_scripts and not the free function, because that
  // hook being wired is the whole of what makes a script reach a model without
  // a game asking. Nothing else would fail if the override were dropped: the
  // module still builds, still attaches, still draws, and the services simply
  // are not there.
  nxe::Module *found = nullptr;
  for (nxe::Module *const module : nxe::enabled_modules())
    if (module != nullptr && module->name() == "live2d")
      found = module;
  REQUIRE(found != nullptr);

  nxe::Engine engine{nullptr};
  script::Host host;
  found->on_expose_scripts(host, engine);

  const Exposed direct;
  CHECK(host.exposed_count() == direct.services.size());
  CHECK(host.exposed_count() > 0u);
}
