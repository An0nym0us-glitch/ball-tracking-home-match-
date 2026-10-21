cmake_minimum_required(VERSION 3.16)
project(pocketeye VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(PE_BUILD_CLI "Build the pocketeye command-line tool" ON)
option(PE_BUILD_TESTS "Build unit tests" ON)
option(PE_BUILD_SHARED_C_API "Build the shared C API library (for dart:ffi / JNI)" ON)

include(FetchContent)

find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d videoio)
find_package(Eigen3 3.3 REQUIRED NO_MODULE)

find_package(nlohmann_json 3.2 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

add_library(pocketeye STATIC
  src/geometry.cpp
  src/ball.cpp
  src/calibration.cpp
  src/lm.cpp
  src/flight.cpp
  src/fit.cpp
  src/detector.cpp
  src/tracker.cpp
  src/analysis.cpp
  src/lbw.cpp
  src/engine.cpp
  src/config.cpp
  src/result_json.cpp
)
target_include_directories(pocketeye PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pocketeye PUBLIC
  opencv_core opencv_imgproc opencv_calib3d
  Eigen3::Eigen nlohmann_json::nlohmann_json)
if(MSVC)
  target_compile_options(pocketeye PRIVATE /W4 /permissive-)
  target_compile_definitions(pocketeye PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
  target_compile_options(pocketeye PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(PE_BUILD_SHARED_C_API)
  add_library(pocketeye_c SHARED src/c_api.cpp)
  target_link_libraries(pocketeye_c PRIVATE pocketeye)
  target_compile_definitions(pocketeye_c PRIVATE PE_BUILDING_C_API)
endif()

if(PE_BUILD_CLI)
  add_executable(pocketeye-cli cli/main.cpp cli/overlay.cpp)
  target_link_libraries(pocketeye-cli PRIVATE pocketeye opencv_videoio)
endif()

if(PE_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
  add_executable(pocketeye-tests
    tests/test_geometry.cpp
    tests/test_calibration.cpp
    tests/test_lm.cpp
    tests/test_flight.cpp
    tests/test_fit.cpp
    tests/test_lbw.cpp
    tests/test_analysis.cpp
    tests/sim.cpp)
  target_link_libraries(pocketeye-tests PRIVATE pocketeye GTest::gtest GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(pocketeye-tests)
endif()
