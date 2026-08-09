# Tests for the Live2D module, built with it.
#
# They link nx::live2d and not the module target: what is under test is the
# runtime, which knows nothing about an Engine.

add_executable(nx_live2d_tests test_live2d_core.cpp test_live2d_assets.cpp
        test_live2d_animate.cpp)

target_link_libraries(nx_live2d_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::compile_options)

# The fixture is not in this repository: the model this was developed against
# is a VTube Studio export of a copyrighted character. Point
# NX_LIVE2D_FIXTURE_DIR at a directory holding a Cubism export to run the cases
# that need one; without it they skip.
set(NX_LIVE2D_FIXTURE_DIR "" CACHE PATH
        "Directory holding a Cubism model export for the Live2D tests")
target_compile_definitions(nx_live2d_tests PRIVATE
        NX_LIVE2D_FIXTURE_DIR="${NX_LIVE2D_FIXTURE_DIR}")

set_target_properties(nx_live2d_tests PROPERTIES FOLDER "tests")

nxtest_discover_tests(nx_live2d_tests LABELS live2d)
