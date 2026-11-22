cmake_minimum_required(VERSION 3.20)
project(neodroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(neodroid_runtime STATIC
  runtime/src/neodroid_runtime.cpp
  runtime/src/neodroid_elf.cpp
  runtime/src/neodroid_vfs.cpp
)

target_include_directories(neodroid_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include
)

target_compile_options(neodroid_runtime PRIVATE
  -Wall -Wextra -Wpedantic -Werror=return-type
)

if(APPLE)
  target_compile_definitions(neodroid_runtime PUBLIC NEODROID_HOST_APPLE=1)
endif()

enable_testing()

add_executable(neodroid_runtime_test tests/runtime_test.cpp)
target_link_libraries(neodroid_runtime_test PRIVATE neodroid_runtime)

add_test(NAME runtime_p0 COMMAND neodroid_runtime_test)

# Build the working syscall-interception proof of concept on ARM64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  add_executable(neodroid_poc_svc pocs/svc_intercept.c)
  add_test(NAME svc_intercept COMMAND neodroid_poc_svc)
endif()
