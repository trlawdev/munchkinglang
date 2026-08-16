include(FetchContent)
include(GoogleTest)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(munx_tests
  compile_run_test.cpp
  analyze_test.cpp
)

target_include_directories(munx_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(munx_tests PRIVATE cxx_std_20)

# GoogleTest needs exceptions/RTTI; munxc itself remains -fno-exceptions.
target_compile_options(munx_tests PRIVATE
  -Wall -Wextra -Wpedantic
)

target_compile_definitions(munx_tests PRIVATE
  MUNXC_EXECUTABLE="$<TARGET_FILE:munxc>"
  MUNX_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(munx_tests PRIVATE
  GTest::gtest_main
)

add_dependencies(munx_tests munxc)

gtest_discover_tests(munx_tests
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  PROPERTIES
    ENVIRONMENT "MUNX_PIPE_HUB=0;MUNXC=$<TARGET_FILE:munxc>;MUNX_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)
