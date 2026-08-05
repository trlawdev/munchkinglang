cmake_minimum_required(VERSION 3.16)

project(munxc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MUNX_BUILD_TESTS "Build GoogleTest suite" ON)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

add_executable(munxc src/main.cpp)
target_include_directories(munxc PRIVATE include)

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
endif()

install(TARGETS munxc RUNTIME DESTINATION bin)

if(MUNX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
