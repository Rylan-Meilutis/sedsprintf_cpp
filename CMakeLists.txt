cmake_minimum_required(VERSION 3.20)
project(sedsprintf_cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_path(ZSTD_INCLUDE_DIR zstd.h HINTS /opt/homebrew/include /usr/local/include /usr/include)
find_library(ZSTD_LIBRARY zstd HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(GTest REQUIRED)
include(GoogleTest)

set(SEDSPRINTF_SCHEMA_PATH "${CMAKE_CURRENT_SOURCE_DIR}/telemetry_config.json" CACHE STRING "Path to telemetry schema JSON")
set(SEDSPRINTF_IPC_SCHEMA_PATH "" CACHE STRING "Optional path to IPC overlay schema JSON")
set(SEDSPRINTF_HEADER_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/header_templates/sedsprintf.h.txt")
set(SEDSPRINTF_ABI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/sedsprintf.h" CACHE STRING "Header source used to extract ABI declarations and result enum")
option(SEDSPRINTF_ENABLE_TIMESYNC "Enable built-in time sync schema generation" ON)
option(SEDSPRINTF_ENABLE_DISCOVERY "Enable built-in discovery schema generation" ON)
set(SEDSPRINTF_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SEDSPRINTF_GENERATED_HEADER "${SEDSPRINTF_GENERATED_DIR}/sedsprintf.h")
set(SEDSPRINTF_GENERATED_SCHEMA_HPP "${SEDSPRINTF_GENERATED_DIR}/generated_schema.hpp")

set(SEDSPRINTF_CODEGEN_ARGS
    --schema "${SEDSPRINTF_SCHEMA_PATH}"
    --header-template "${SEDSPRINTF_HEADER_TEMPLATE}"
    --header-out "${SEDSPRINTF_GENERATED_HEADER}"
    --schema-out "${SEDSPRINTF_GENERATED_SCHEMA_HPP}"
    --abi-source "${SEDSPRINTF_ABI_SOURCE}"
    --result-source "${SEDSPRINTF_ABI_SOURCE}"
    --enable-timesync "$<IF:$<BOOL:${SEDSPRINTF_ENABLE_TIMESYNC}>,1,0>"
    --enable-discovery "$<IF:$<BOOL:${SEDSPRINTF_ENABLE_DISCOVERY}>,1,0>"
)
if(SEDSPRINTF_IPC_SCHEMA_PATH)
  list(APPEND SEDSPRINTF_CODEGEN_ARGS --ipc-schema "${SEDSPRINTF_IPC_SCHEMA_PATH}")
endif()

add_custom_command(
  OUTPUT "${SEDSPRINTF_GENERATED_HEADER}" "${SEDSPRINTF_GENERATED_SCHEMA_HPP}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SEDSPRINTF_GENERATED_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_schema.py"
          ${SEDSPRINTF_CODEGEN_ARGS}
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_schema.py"
    "${SEDSPRINTF_SCHEMA_PATH}"
    "${SEDSPRINTF_HEADER_TEMPLATE}"
    "${SEDSPRINTF_ABI_SOURCE}"
  VERBATIM
)
if(SEDSPRINTF_IPC_SCHEMA_PATH)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SEDSPRINTF_IPC_SCHEMA_PATH}")
endif()

add_custom_target(sedsprintf_codegen DEPENDS "${SEDSPRINTF_GENERATED_HEADER}" "${SEDSPRINTF_GENERATED_SCHEMA_HPP}")

add_library(sedsprintf_cpp STATIC
  src/core.cpp
  src/discovery.cpp
  src/packet.cpp
  src/queue.cpp
  src/relay.cpp
  src/router.cpp
  src/serialize.cpp
  src/router_core.cpp
  src/packet_api.cpp
  src/router_api.cpp
  src/relay_api.cpp
  src/timesync.cpp
)
add_dependencies(sedsprintf_cpp sedsprintf_codegen)
target_include_directories(sedsprintf_cpp PUBLIC ${SEDSPRINTF_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
if(ZSTD_INCLUDE_DIR)
  target_include_directories(sedsprintf_cpp PRIVATE ${ZSTD_INCLUDE_DIR})
endif()
if(ZSTD_LIBRARY)
  target_link_libraries(sedsprintf_cpp PRIVATE ${ZSTD_LIBRARY})
  target_compile_definitions(sedsprintf_cpp PRIVATE SEDS_HAS_ZSTD=1)
endif()

enable_testing()

add_executable(sedsprintf_cpp_tests tests/tests.cpp)
add_dependencies(sedsprintf_cpp_tests sedsprintf_codegen)
target_link_libraries(sedsprintf_cpp_tests PRIVATE sedsprintf_cpp GTest::gtest_main)
gtest_discover_tests(sedsprintf_cpp_tests)

add_executable(sedsprintf_cpp_wrapper_tests tests/test_cpp_wrappers.cpp)
add_dependencies(sedsprintf_cpp_wrapper_tests sedsprintf_codegen)
target_link_libraries(sedsprintf_cpp_wrapper_tests PRIVATE sedsprintf_cpp GTest::gtest_main)
gtest_discover_tests(sedsprintf_cpp_wrapper_tests)

set(SEDSPRINTF_OVERLAY_SCHEMA_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tests/schemas/ipc_link_local_overlay.json")
set(SEDSPRINTF_OVERLAY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_overlay")
set(SEDSPRINTF_OVERLAY_GENERATED_HEADER "${SEDSPRINTF_OVERLAY_GENERATED_DIR}/sedsprintf.h")
set(SEDSPRINTF_OVERLAY_GENERATED_SCHEMA_HPP "${SEDSPRINTF_OVERLAY_GENERATED_DIR}/generated_schema.hpp")
set(SEDSPRINTF_OVERLAY_CODEGEN_ARGS
    --schema "${SEDSPRINTF_SCHEMA_PATH}"
    --ipc-schema "${SEDSPRINTF_OVERLAY_SCHEMA_PATH}"
    --header-template "${SEDSPRINTF_HEADER_TEMPLATE}"
    --header-out "${SEDSPRINTF_OVERLAY_GENERATED_HEADER}"
    --schema-out "${SEDSPRINTF_OVERLAY_GENERATED_SCHEMA_HPP}"
    --abi-source "${SEDSPRINTF_ABI_SOURCE}"
    --result-source "${SEDSPRINTF_ABI_SOURCE}"
    --enable-timesync "$<IF:$<BOOL:${SEDSPRINTF_ENABLE_TIMESYNC}>,1,0>"
    --enable-discovery "$<IF:$<BOOL:${SEDSPRINTF_ENABLE_DISCOVERY}>,1,0>"
)
add_custom_command(
  OUTPUT "${SEDSPRINTF_OVERLAY_GENERATED_HEADER}" "${SEDSPRINTF_OVERLAY_GENERATED_SCHEMA_HPP}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SEDSPRINTF_OVERLAY_GENERATED_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_schema.py"
          ${SEDSPRINTF_OVERLAY_CODEGEN_ARGS}
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_schema.py"
    "${SEDSPRINTF_SCHEMA_PATH}"
    "${SEDSPRINTF_OVERLAY_SCHEMA_PATH}"
    "${SEDSPRINTF_HEADER_TEMPLATE}"
    "${SEDSPRINTF_ABI_SOURCE}"
  VERBATIM
)
add_custom_target(sedsprintf_codegen_overlay DEPENDS "${SEDSPRINTF_OVERLAY_GENERATED_HEADER}" "${SEDSPRINTF_OVERLAY_GENERATED_SCHEMA_HPP}")

add_library(sedsprintf_cpp_overlay STATIC
  src/core.cpp
  src/discovery.cpp
  src/packet.cpp
  src/queue.cpp
  src/relay.cpp
  src/router.cpp
  src/serialize.cpp
  src/router_core.cpp
  src/packet_api.cpp
  src/router_api.cpp
  src/relay_api.cpp
  src/timesync.cpp
)
add_dependencies(sedsprintf_cpp_overlay sedsprintf_codegen_overlay)
target_include_directories(sedsprintf_cpp_overlay PUBLIC ${SEDSPRINTF_OVERLAY_GENERATED_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sedsprintf_cpp_overlay PRIVATE SEDSPRINTF_USE_OVERLAY_SCHEMA=1)
if(ZSTD_INCLUDE_DIR)
  target_include_directories(sedsprintf_cpp_overlay PRIVATE ${ZSTD_INCLUDE_DIR})
endif()
if(ZSTD_LIBRARY)
  target_link_libraries(sedsprintf_cpp_overlay PRIVATE ${ZSTD_LIBRARY})
  target_compile_definitions(sedsprintf_cpp_overlay PRIVATE SEDS_HAS_ZSTD=1)
endif()

add_executable(sedsprintf_cpp_overlay_tests tests/test_link_local_overlay.cpp)
add_dependencies(sedsprintf_cpp_overlay_tests sedsprintf_codegen_overlay)
target_link_libraries(sedsprintf_cpp_overlay_tests PRIVATE sedsprintf_cpp_overlay GTest::gtest_main)
gtest_discover_tests(sedsprintf_cpp_overlay_tests)

add_executable(sedsprintf_c_interop_test tests/c_interop_test.c)
add_dependencies(sedsprintf_c_interop_test sedsprintf_codegen)
target_link_libraries(sedsprintf_c_interop_test PRIVATE sedsprintf_cpp)
add_test(NAME sedsprintf_c_interop_test COMMAND sedsprintf_c_interop_test)

add_executable(sedsprintf_c_system_test tests/c_system_test.c)
add_dependencies(sedsprintf_c_system_test sedsprintf_codegen)
target_link_libraries(sedsprintf_c_system_test PRIVATE sedsprintf_cpp)
add_test(NAME sedsprintf_c_system_test COMMAND sedsprintf_c_system_test)

add_test(NAME sedsprintf_codegen_check
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_codegen.py)
