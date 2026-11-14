# PTZ Control for Linux: Qt 6 Widgets host over the shared C++ core.
# linux/install.sh builds it inside the Flatpak build (linux/org.ptzcontrol.app.yml).
cmake_minimum_required(VERSION 3.16)
project(PTZControl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(CURL REQUIRED)
# The live view decodes with the Flatpak runtime's GStreamer (codecs from org.freedesktop.Platform.codecs-extra).
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

file(GLOB CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/*.cpp)
add_executable(ptz-control
    ${CORE_SOURCES}
    src/main.cpp src/support.cpp src/instruments.cpp src/console.cpp src/vault.cpp src/live.cpp)
target_include_directories(ptz-control PRIVATE ../core/include src)
target_compile_options(ptz-control PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(ptz-control PRIVATE Qt6::Widgets CURL::libcurl PkgConfig::GST)
