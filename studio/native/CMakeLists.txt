cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
project(StudioNative)
# Studio's two command-line tools (macOS / Linux). Studio runs them from
# studio/native/build/.
#   cmake -S studio/native -B studio/native/build && cmake --build studio/native/build
# Default to an optimized build: with no build type CMake compiles WITHOUT optimization,
# and the per-pixel loops here (blend, seam search, map building) run several times slower.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
find_package(OpenCV REQUIRED)
include_directories(${OpenCV_INCLUDE_DIRS} ${PROJECT_SOURCE_DIR}/include)

# StitchPipeline: calibration-driven, feature-free cylindrical stitch of a camera pair.
add_executable(StitchPipeline stitch_pipeline.cpp)
target_link_libraries(StitchPipeline ${OpenCV_LIBS})

# Director: renders a virtual-camera edit (made in Studio's editor) of a stitched video.
add_executable(Director director.cpp)
target_link_libraries(Director ${OpenCV_LIBS})
