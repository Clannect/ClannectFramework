# Header-only helpers shared by every module's tests.
add_library(cfw-test-support INTERFACE)
target_include_directories(cfw-test-support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
