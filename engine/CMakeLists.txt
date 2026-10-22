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

option(PE_WITH_VIDEOIO "Video file support (analysis, replay video, frame reader)" ON)

set(PE_OPENCV_COMPONENTS core imgproc calib3d imgcodecs)
if(PE_WITH_VIDEOIO)
  list(APPEND PE_OPENCV_COMPONENTS videoio)
endif()
find_package(OpenCV REQUIRED COMPONENTS ${PE_OPENCV_COMPONENTS})

find_package(Eigen3 3.3 QUIET NO_MODULE)
if(NOT TARGET Eigen3::Eigen)
  # Header-only: fetch the sources and expose an interface target.
  FetchContent_Declare(eigen URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz)
  FetchContent_GetProperties(eigen)
  if(NOT eigen_POPULATED)
    FetchContent_Populate(eigen)
  endif()
  add_library(Eigen3::Eigen INTERFACE IMPORTED)
  set_target_properties(Eigen3::Eigen PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${eigen_SOURCE_DIR})
endif()

find_package(nlohmann_json 3.2 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

set(PE_SOURCES
  src/geometry.cpp
  src/ball.cpp
  src/calibration.cpp
  src/autocal.cpp
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
  src/benchmark.cpp
)
if(PE_WITH_VIDEOIO)
  list(APPEND PE_SOURCES src/overlay.cpp src/video.cpp)
endif()
add_library(pocketeye STATIC ${PE_SOURCES})
target_include_directories(pocketeye PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${OpenCV_INCLUDE_DIRS})
target_link_libraries(pocketeye PUBLIC ${OpenCV_LIBS} Eigen3::Eigen nlohmann_json::nlohmann_json)
if(PE_WITH_VIDEOIO)
  target_compile_definitions(pocketeye PUBLIC PE_WITH_VIDEOIO)
endif()
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
  if(ANDROID)
    target_link_libraries(pocketeye_c PRIVATE log)
    target_link_options(pocketeye_c PRIVATE "-Wl,-z,max-page-size=16384")
  endif()
endif()

if(PE_BUILD_CLI AND PE_WITH_VIDEOIO)
  add_executable(pocketeye-cli cli/main.cpp)
  target_link_libraries(pocketeye-cli PRIVATE pocketeye)
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
    tests/test_engine.cpp
    tests/test_autocal.cpp
    tests/sim.cpp)
  target_link_libraries(pocketeye-tests PRIVATE pocketeye GTest::gtest GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(pocketeye-tests)
endif()
