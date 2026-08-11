# Tests for the Live2D module, built with it.
#
# They link nx::live2d and not the module target: what is under test is the
# runtime, which knows nothing about an Engine.

add_executable(nx_live2d_tests test_live2d_core.cpp test_live2d_assets.cpp
        test_live2d_animate.cpp test_live2d_draw.cpp test_live2d_mask.cpp
        test_live2d_render.cpp test_live2d_clip.cpp test_live2d_pass.cpp
        test_live2d_scene.cpp test_live2d_effects.cpp test_live2d_module.cpp
        test_live2d_scripting.cpp)

# The attach case needs the module and an Engine to attach it to, and the
# generated registry that hands modules over. Everything else here links only
# the runtime.
nx_module_registry(nx_live2d_tests)

# nx::rhi and nx::stb for the one case that draws. nx_live2d links neither:
# proving a model reaches a framebuffer needs a device to reach, and a PNG
# writer to look at what landed.
target_link_libraries(nx_live2d_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::live2d_render nx::rhi
        nx::stb nx::engine nx::compile_options)

# The fixture is not in this repository: the model this was developed against
# is a VTube Studio export of a copyrighted character. Point
# NX_LIVE2D_FIXTURE_DIR at a directory holding a Cubism export to run the cases
# that need one; without it they skip.
set(NX_LIVE2D_FIXTURE_DIR "" CACHE PATH
        "Directory holding a Cubism model export for the Live2D tests")
if (ANDROID)
    set(NX_TEST_SHADERS "${NX_ANDROID_SHADER_OUT}")
else ()
    set(NX_TEST_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
endif ()
target_compile_definitions(nx_live2d_tests PRIVATE
        NX_LIVE2D_FIXTURE_DIR="${NX_LIVE2D_FIXTURE_DIR}"
        NX_TEST_SHADER_DIR="${NX_TEST_SHADERS}"
        # What the attach case mounts, so Engine::load_shader finds the
        # module's own shader the way a game would.
        NX_TEST_RUNTIME_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if (TARGET nx_rendering_shaders)
    add_dependencies(nx_live2d_tests nx_rendering_shaders)
endif ()
# The clip case loads the module's own shader, which nx_module() compiles into
# a target of its own.
if (TARGET nx_module_live2d_shaders)
    add_dependencies(nx_live2d_tests nx_module_live2d_shaders)
endif ()

set_target_properties(nx_live2d_tests PROPERTIES FOLDER "tests")

# These are the proprietary-fixture cases: written against one development
# model's exact numbers, so they skip without NX_LIVE2D_FIXTURE_DIR. A separate
# label keeps them off a required CI run, which leans on the bundled suite below.
nxtest_discover_tests(nx_live2d_tests LABELS live2d-proprietary)

# The always-on suite. Hiyori is a free Live2D sample that ships under the module
# template; it is staged into a mountable layout so these run out of a plain
# checkout, asserting only what any Cubism export must have.
set(_live2d_bundled "${CMAKE_CURRENT_BINARY_DIR}/bundled")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../templates/model/assets/live2d"
        DESTINATION "${_live2d_bundled}")

add_executable(nx_live2d_bundled_tests test_live2d_bundled.cpp)
target_link_libraries(nx_live2d_bundled_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::compile_options)
target_compile_definitions(nx_live2d_bundled_tests PRIVATE
        NX_LIVE2D_BUNDLED_DIR="${_live2d_bundled}"
        NX_REQUIRE_MODULE_FIXTURES=$<BOOL:${NX_REQUIRE_MODULE_FIXTURES}>)
set_target_properties(nx_live2d_bundled_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_live2d_bundled_tests LABELS live2d)
