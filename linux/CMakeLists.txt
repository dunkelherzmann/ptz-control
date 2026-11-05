# PTZ Control for Linux: Qt 6 Widgets host over the shared C++ core.
# scripts/build-linux.sh runs this inside the build container and packages AppImages.
cmake_minimum_required(VERSION 3.16)
project(PTZControl VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(CURL REQUIRED)

file(GLOB CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../core/src/*.cpp)
add_executable(ptz-control
    ${CORE_SOURCES}
    src/main.cpp src/support.cpp src/instruments.cpp src/console.cpp)
target_include_directories(ptz-control PRIVATE ../core/include src)
target_compile_options(ptz-control PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(ptz-control PRIVATE Qt6::Widgets CURL::libcurl)
# Bundled libraries live in <AppDir>/usr/lib next to usr/bin.
set_target_properties(ptz-control PROPERTIES INSTALL_RPATH "$ORIGIN/../lib" BUILD_WITH_INSTALL_RPATH ON)
