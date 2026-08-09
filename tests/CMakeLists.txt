# Tests for the Live2D module, built with it.
#
# They link nx::live2d and not the module target: what is under test is the
# runtime, which knows nothing about an Engine.

add_executable(nx_live2d_tests test_live2d_core.cpp test_live2d_assets.cpp
        test_live2d_animate.cpp test_live2d_draw.cpp test_live2d_mask.cpp
        test_live2d_render.cpp test_live2d_clip.cpp test_live2d_pass.cpp
        test_live2d_scene.cpp)

# nx::rhi and nx::stb for the one case that draws. nx_live2d links neither:
# proving a model reaches a framebuffer needs a device to reach, and a PNG
# writer to look at what landed.
target_link_libraries(nx_live2d_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::live2d_render nx::rhi
        nx::stb nx::compile_options)

# The fixture is not in this repository: the model this was developed against
# is a VTube Studio export of a copyrighted character. Point
# NX_LIVE2D_FIXTURE_DIR at a directory holding a Cubism export to run the cases
# that need one; without it they skip.
set(NX_LIVE2D_FIXTURE_DIR "" CACHE PATH
        "Directory holding a Cubism model export for the Live2D tests")
if (ANDROID)
    set(NX_TEST_SHADERS "${CMAKE_SOURCE_DIR}/android/app/src/main/assets/shaders")
else ()
    set(NX_TEST_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
endif ()
target_compile_definitions(nx_live2d_tests PRIVATE
        NX_LIVE2D_FIXTURE_DIR="${NX_LIVE2D_FIXTURE_DIR}"
        NX_TEST_SHADER_DIR="${NX_TEST_SHADERS}")
if (TARGET nx_rendering_shaders)
    add_dependencies(nx_live2d_tests nx_rendering_shaders)
endif ()
# The clip case loads the module's own shader, which nx_module() compiles into
# a target of its own.
if (TARGET nx_module_live2d_shaders)
    add_dependencies(nx_live2d_tests nx_module_live2d_shaders)
endif ()

set_target_properties(nx_live2d_tests PROPERTIES FOLDER "tests")

nxtest_discover_tests(nx_live2d_tests LABELS live2d)
