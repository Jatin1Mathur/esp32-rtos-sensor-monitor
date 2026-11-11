# Host build: compiles the platform-independent core library and runs the
# unit tests + threaded simulation on Linux/macOS. The firmware itself is
# built with PlatformIO (see platformio.ini).
cmake_minimum_required(VERSION 3.16)
project(esp32_rtos_sensor_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SANITIZE "Sanitizer to enable: address, thread or empty" "")

add_compile_options(-Wall -Wextra -Wpedantic -Werror)
if(SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()

add_library(sm_core
  lib/sm_core/src/sht3x.cpp
  lib/sm_core/src/supervisor.cpp
  lib/sm_core/src/json_encoder.cpp
  lib/sm_core/src/tasks.cpp)
target_include_directories(sm_core PUBLIC lib/sm_core/include)

include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(host_tests
  host_tests/test_sht3x.cpp
  host_tests/test_supervisor.cpp
  host_tests/test_tasks.cpp
  host_tests/test_pipeline_sim.cpp
  host_tests/test_dataset_replay.cpp)
target_include_directories(host_tests PRIVATE host_tests/support)
target_compile_definitions(host_tests PRIVATE DATASET_CSV="${CMAKE_SOURCE_DIR}/data/office_room_2015.csv")
target_link_libraries(host_tests PRIVATE sm_core GTest::gmock_main)

include(GoogleTest)
gtest_discover_tests(host_tests)
