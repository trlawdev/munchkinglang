cmake_minimum_required(VERSION 3.16)

project(munxc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MUNX_BUILD_TESTS "Build GoogleTest suite" ON)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

# custom: MIR→C via host cc
# llvm:   MIR→LLVM IR text, codegen/link via clang (no libLLVM link into munxc)
# both:   both backends; select with --backend
set(MUNX_NATIVE_BACKEND "custom" CACHE STRING "Native backend: custom | llvm | both")
set_property(CACHE MUNX_NATIVE_BACKEND PROPERTY STRINGS custom llvm both)

add_executable(munxc src/main.cpp)
target_include_directories(munxc PRIVATE include)

set(MUNX_NATIVE_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/native/runtime")
target_compile_definitions(munxc PRIVATE
  "MUNX_NATIVE_RUNTIME_DIR=\"${MUNX_NATIVE_RUNTIME_DIR}\"")

if(MUNX_NATIVE_BACKEND STREQUAL "custom")
  target_compile_definitions(munxc PRIVATE MUNX_NATIVE_CUSTOM=1 MUNX_NATIVE_LLVM=0)
elseif(MUNX_NATIVE_BACKEND STREQUAL "llvm")
  target_compile_definitions(munxc PRIVATE MUNX_NATIVE_CUSTOM=0 MUNX_NATIVE_LLVM=1)
elseif(MUNX_NATIVE_BACKEND STREQUAL "both")
  target_compile_definitions(munxc PRIVATE MUNX_NATIVE_CUSTOM=1 MUNX_NATIVE_LLVM=1)
else()
  message(FATAL_ERROR "MUNX_NATIVE_BACKEND must be custom, llvm, or both")
endif()

if(MSVC)
  target_compile_options(munxc PRIVATE /W4 /GR- /EHs-c-)
else()
  target_compile_options(munxc PRIVATE
    -Wall -Wextra -Wpedantic -pthread
    -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections
  )
  if(NOT APPLE)
    target_link_options(munxc PRIVATE -Wl,--gc-sections)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i686)$")
    target_compile_options(munxc PRIVATE -mavx2 -mfma)
  endif()
endif()

if(WIN32)
  target_link_libraries(munxc PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(munxc PRIVATE Threads::Threads)
  if(UNIX AND NOT APPLE)
    target_link_libraries(munxc PRIVATE dl)
  endif()
endif()

install(TARGETS munxc RUNTIME DESTINATION bin)

if(MUNX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
