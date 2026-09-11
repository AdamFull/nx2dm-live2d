
# Cubism ships in two parts under separate licenses, and only one of them can
# live in this repo. The Framework (Live2D Open Software License) is plain
# C++ source hosted at github.com/Live2D/CubismNativeFramework, so it is a
# normal pinned git submodule - see third_party/CubismNativeFramework. The
# Core (Live2D Proprietary Software License) is a prebuilt binary Live2D
# distributes only behind a licence-acceptance click-through on their own
# site, with no fetchable URL; it can never be committed or auto-downloaded,
# so it stays a required local, gitignored, developer-provided directory.

set(NX_LIVE2D_CORE_DIR "" CACHE PATH
        "An extracted Cubism Core (Core/include/Live2DCubismCore.h, or that directory itself). Empty uses modules/live2d/third_party/CubismCore.")

# The exact release this was written against. A mismatch is not fatal - the
# Core ABI is stable across a release line - but it is worth saying out loud
# when a build is not using what anybody tested.
set(NX_LIVE2D_SDK_VERSION "5-r.5")

function(_nx_live2d_resolve_core root out_var)
    foreach (candidate "${root}/CubismCore" "${root}/Core" "${root}/CubismSdkForNative/Core" "${root}")
        if (EXISTS "${candidate}/include/Live2DCubismCore.h")
            set(${out_var} "${candidate}" PARENT_SCOPE)
            return()
        endif ()
    endforeach ()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(nx_add_live2d)
    if (NX_LIVE2D_CORE_DIR)
        _nx_live2d_resolve_core("${NX_LIVE2D_CORE_DIR}" _core)
        if (NOT _core)
            message(FATAL_ERROR
                    "NX_LIVE2D_CORE_DIR='${NX_LIVE2D_CORE_DIR}' holds no Cubism "
                    "Core: expected include/Live2DCubismCore.h there, or under "
                    "Core/ or CubismSdkForNative/Core/.")
        endif ()
    else ()
        _nx_live2d_resolve_core("${CMAKE_CURRENT_LIST_DIR}/third_party" _core)
        if (NOT _core)
            message(FATAL_ERROR
                    "nx2d: no Cubism Core. Download the Cubism SDK for Native "
                    "(${NX_LIVE2D_SDK_VERSION}) from https://www.live2d.com/en/sdk/"
                    "download/native/, then put its Core/ directory at "
                    "modules/live2d/third_party/CubismCore (or point "
                    "NX_LIVE2D_CORE_DIR at it). It is not fetchable here: Live2D "
                    "distributes it only behind their licence acceptance. Never "
                    "commit it - modules/live2d/third_party/CubismCore is "
                    "gitignored on purpose.")
        endif ()
    endif ()

    _nx_prebuilt_paths("${_core}" "Live2DCubismCore" _core_debug _core_release)
    if (NOT EXISTS "${_core_release}")
        message(FATAL_ERROR "nx2d: no Cubism Core binary at ${_core_release}.")
    endif ()
    if (NOT EXISTS "${_core_debug}")
        set(_core_debug "${_core_release}")
    endif ()

    add_library(nx_live2d_core STATIC IMPORTED GLOBAL)
    add_library(nx::live2d_core ALIAS nx_live2d_core)
    set_target_properties(nx_live2d_core PROPERTIES
            IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
            IMPORTED_LOCATION "${_core_release}"
            IMPORTED_LOCATION_DEBUG "${_core_debug}"
            IMPORTED_LOCATION_RELEASE "${_core_release}"
            MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
            MAP_IMPORTED_CONFIG_MINSIZEREL Release
            INTERFACE_INCLUDE_DIRECTORIES "${_core}/include")

    set(_framework "${CMAKE_CURRENT_LIST_DIR}/third_party/CubismNativeFramework")
    if (NOT EXISTS "${_framework}/src/CubismFramework.cpp")
        message(FATAL_ERROR
                "nx2d: no Cubism Framework at ${_framework}. Run "
                "'git submodule update --init -- modules/live2d/third_party/"
                "CubismNativeFramework'.")
    endif ()
    set(_src "${_framework}/src")
    file(GLOB _framework_sources CONFIGURE_DEPENDS
            "${_src}/*.cpp"
            "${_src}/Effect/*.cpp"
            "${_src}/Id/*.cpp"
            "${_src}/Math/*.cpp"
            "${_src}/Model/*.cpp"
            "${_src}/Motion/*.cpp"
            "${_src}/Physics/*.cpp"
            "${_src}/Rendering/*.cpp"
            "${_src}/Type/*.cpp"
            "${_src}/Utils/*.cpp")
    if (NOT _framework_sources)
        message(FATAL_ERROR "no Cubism Framework sources under ${_src}")
    endif ()

    # Our own target rather than the SDK's CMakeLists, for the reason
    # NxSpine.cmake gives: theirs sets flags of its own choosing, and this way
    # the framework is built like everything else in the tree.
    add_library(nx_live2d_framework STATIC ${_framework_sources})
    add_library(nx::live2d_framework ALIAS nx_live2d_framework)
    target_include_directories(nx_live2d_framework SYSTEM PUBLIC "${_src}")
    target_link_libraries(nx_live2d_framework PUBLIC nx::live2d_core)
    set_target_properties(nx_live2d_framework PROPERTIES FOLDER "third_party")

    message(STATUS "nx2d: Live2D Core from ${_core}, Framework from ${_framework}")
    set(NX_LIVE2D_CORE_RESOLVED_DIR "${_core}" CACHE INTERNAL "resolved Cubism Core root")
endfunction()
