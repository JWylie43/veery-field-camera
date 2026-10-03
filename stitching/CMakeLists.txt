cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(StitchingApplication)
# Default to an optimized build: with no build type CMake compiles WITHOUT optimization,
# and the per-pixel loops here (blend, seam search, map building) run several times slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(WIN32)
  set(OpenCV_DIR "C:/opencv/build")
endif()
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include)

# Calibration-driven, feature-free pipeline (builds on OpenCV 4.x AND 5.x)
add_executable(StitchPipeline stitch_pipeline.cpp)
target_link_libraries(StitchPipeline ${OpenCV_LIBS})
if(WIN32)
  target_link_libraries(StitchPipeline ws2_32)   # sockets for the --tune web UI
endif()