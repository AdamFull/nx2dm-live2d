
option(NX_MODULE_LIVE2D "Build the Live2D Cubism integration" OFF)

# Model cooking uses only foundation JSON/NVA code, so the host asset compiler
# can support Cubism projects without linking the proprietary target runtime.
nx_asset_cooker(live2d
        SOURCES live2d_cooker.cpp live2d_asset_bundle.cpp)

if (NOT NX_MODULE_LIVE2D)
    return()
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/NxLive2D.cmake")
nx_add_live2d()

set(NX_LIVE2D_HEADERS live2d_animator.h live2d_assets.h live2d_component.h
        live2d_draw.h live2d_mask.h live2d_platform.h live2d_asset_bundle.h)
set(NX_LIVE2D_SOURCES live2d_animator.cpp live2d_assets.cpp live2d_draw.cpp
        live2d_mask.cpp live2d_platform.cpp live2d_renderer.cpp
        live2d_asset_bundle.cpp)

add_library(nx_live2d STATIC ${NX_LIVE2D_SOURCES} ${NX_LIVE2D_HEADERS})
add_library(nx::live2d ALIAS nx_live2d)

target_include_directories(nx_live2d PUBLIC "${NX_ENGINE_ROOT}/engine"
        "${NX_ENGINE_ROOT}/modules")

target_link_libraries(nx_live2d
        PUBLIC nx::foundation nx::render2d nx::live2d_framework
        PRIVATE nx::compile_options)

# live2d_renderer.cpp defines the CubismRenderer statics the framework calls,
# and nothing else in nx_live2d references that object. GNU ld scans archives
# once, left to right, so unless nx_live2d follows the framework on the link
# line too, the object is never pulled in and the link fails. CMake repeats the
# pair when two static libraries depend on each other.
target_link_libraries(nx_live2d_framework PUBLIC nx_live2d)

set_target_properties(nx_live2d PROPERTIES FOLDER "modules")

add_library(nx_live2d_render STATIC live2d_pass.cpp live2d_pass.h
        live2d_system.cpp live2d_system.h)
add_library(nx::live2d_render ALIAS nx_live2d_render)

target_include_directories(nx_live2d_render PUBLIC "${NX_ENGINE_ROOT}/engine"
        "${NX_ENGINE_ROOT}/modules")

target_link_libraries(nx_live2d_render
        PUBLIC nx::live2d nx::rhi nx::render_graph
        PRIVATE nx::compile_options)

set_target_properties(nx_live2d_render PROPERTIES FOLDER "modules")

nx_module(live2d
        DISPLAY "the Live2D Cubism integration"
        SOURCES live2d_module.cpp live2d_scripting.cpp
        DEPENDS nx::live2d_render)

if (NX_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
