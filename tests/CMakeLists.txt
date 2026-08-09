# Tests for the Live2D module, built with it.
#
# They link nx::live2d and not the module target: what is under test is the
# runtime, which knows nothing about an Engine.

add_executable(nx_live2d_tests test_live2d_core.cpp)

target_link_libraries(nx_live2d_tests
        PRIVATE nx::test nx::foundation nx::live2d nx::compile_options)

set_target_properties(nx_live2d_tests PROPERTIES FOLDER "tests")

nxtest_discover_tests(nx_live2d_tests LABELS live2d)
