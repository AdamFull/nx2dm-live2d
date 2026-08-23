
add_executable(nx_live2d_tests test_live2d_core.cpp test_live2d_assets.cpp
        test_live2d_animate.cpp test_live2d_draw.cpp test_live2d_mask.cpp
        test_live2d_render.cpp test_live2d_clip.cpp test_live2d_pass.cpp
        test_live2d_scene.cpp test_live2d_effects.cpp test_live2d_module.cpp
        test_live2d_scripting.cpp)

nx_module_registry(nx_live2d_tests)

target_link_libraries(nx_live2d_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::live2d_render nx::rhi
        nx::stb nx::engine nx::compile_options)

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
        NX_TEST_RUNTIME_DIR="${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if (TARGET nx_rendering_shaders)
    add_dependencies(nx_live2d_tests nx_rendering_shaders)
endif ()
if (TARGET nx_module_live2d_shaders)
    add_dependencies(nx_live2d_tests nx_module_live2d_shaders)
endif ()

set_target_properties(nx_live2d_tests PROPERTIES FOLDER "tests")

nxtest_discover_tests(nx_live2d_tests LABELS live2d-proprietary)

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
