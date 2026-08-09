# The Live2D Cubism runtime, as a module. See docs/live2d.md.
#
# Two targets, as Spine has. nx_live2d is the runtime and links no engine and no
# RHI, so loading and deforming a model is testable headless; nx_module_live2d
# is the handful of lines that tell an Engine about it.
#
# The option is declared here rather than left to nx_module() because the SDK
# has to be found before the module can link it, and a build without one should
# say so rather than fail in the linker.

option(NX_MODULE_LIVE2D "Build the Live2D Cubism integration" OFF)
if (NOT NX_MODULE_LIVE2D)
    return()
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/NxLive2D.cmake")
nx_add_live2d()

set(NX_LIVE2D_HEADERS live2d_animator.h live2d_assets.h live2d_draw.h
        live2d_mask.h live2d_platform.h)
set(NX_LIVE2D_SOURCES live2d_animator.cpp live2d_assets.cpp live2d_draw.cpp
        live2d_mask.cpp live2d_platform.cpp live2d_renderer.cpp)

add_library(nx_live2d STATIC ${NX_LIVE2D_SOURCES} ${NX_LIVE2D_HEADERS})
add_library(nx::live2d ALIAS nx_live2d)

target_include_directories(nx_live2d PUBLIC "${CMAKE_SOURCE_DIR}/engine"
        "${CMAKE_SOURCE_DIR}/modules")

# render2d for the packed bindless word a texture resolves to, and the mesh
# channel L3 emits into. No RHI and no engine: a model is geometry until
# something draws it.
target_link_libraries(nx_live2d
        PUBLIC nx::foundation nx::render2d nx::live2d_framework
        PRIVATE nx::compile_options)

set_target_properties(nx_live2d PROPERTIES FOLDER "modules")

nx_module(live2d
        DISPLAY "the Live2D Cubism integration"
        SOURCES live2d_module.cpp
        DEPENDS nx::live2d)

if (NX_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
