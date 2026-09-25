
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

add_executable(nx_live2d_bundled_tests test_live2d_bundled.cpp
        test_live2d_asset_bundle.cpp test_live2d_loading.cpp)
target_link_libraries(nx_live2d_bundled_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::live2d_render
        nx::compile_options)
target_compile_definitions(nx_live2d_bundled_tests PRIVATE
        NX_LIVE2D_BUNDLED_DIR="${_live2d_bundled}"
        NX_REQUIRE_MODULE_FIXTURES=$<BOOL:${NX_REQUIRE_MODULE_FIXTURES}>)
set_target_properties(nx_live2d_bundled_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_live2d_bundled_tests LABELS live2d)

if (TARGET assetc AND NOT ANDROID AND NOT IOS)
    set(_live2d_cooked "${CMAKE_CURRENT_BINARY_DIR}/cooked")
    add_custom_target(nx_live2d_test_cooked_assets
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_live2d_cooked}"
            COMMAND "$<TARGET_FILE:assetc>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../templates/model/assets"
                    "${_live2d_cooked}"
                    "${CMAKE_CURRENT_BINARY_DIR}/cooked.manifest"
                    --root / --shipping --require-metadata
            DEPENDS assetc
            COMMENT "Cooking the bundled Live2D model for runtime tests"
            VERBATIM)
    set_target_properties(nx_live2d_test_cooked_assets PROPERTIES FOLDER "tests")

    add_executable(nx_live2d_shipping_asset_tests
            test_live2d_assets_shipping.cpp
            ../live2d_assets.cpp ../live2d_asset_bundle.cpp
            ../live2d_platform.cpp ../live2d_renderer.cpp)
    target_include_directories(nx_live2d_shipping_asset_tests PRIVATE
            "${NX_ENGINE_ROOT}/engine" "${NX_ENGINE_ROOT}/modules")
    target_link_libraries(nx_live2d_shipping_asset_tests
            PRIVATE nx::test nx::foundation nx::render2d
            nx::live2d_framework nx::compile_options)
    target_compile_definitions(nx_live2d_shipping_asset_tests PRIVATE
            NX_BUILD_SHIPPING=1
            NX_LIVE2D_COOKED_DIR="${_live2d_cooked}"
            NX_LIVE2D_AUTHORED_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../templates/model/assets")
    add_dependencies(nx_live2d_shipping_asset_tests
            nx_live2d_test_cooked_assets)
    set_target_properties(nx_live2d_shipping_asset_tests PROPERTIES
            FOLDER "tests")
    nxtest_discover_tests(nx_live2d_shipping_asset_tests
            PREFIX "shipping: " LABELS live2d)
endif ()

target_compile_definitions(nx_live2d_tests PRIVATE
        NX_MODULE_SERVICES_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../script-services.json")
target_link_libraries(nx_live2d_tests PRIVATE nx::script_luau)
