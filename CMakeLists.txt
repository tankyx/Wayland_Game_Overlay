cmake_minimum_required(VERSION 3.16)
project(wayland_game_overlay C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)

add_library(wayland_overlay STATIC
    src/WaylandOverlay.cpp
    src/wayland/xdg-shell-protocol.c
    src/wayland/wlr-layer-shell-unstable-v1-protocol.c
)
target_include_directories(wayland_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wayland_overlay PUBLIC PkgConfig::WAYLAND pthread)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(example)

    enable_testing()
    add_executable(raster_test tests/raster_test.cpp)
    target_include_directories(raster_test PRIVATE src)
    add_test(NAME raster_test COMMAND raster_test)

    add_executable(menu_test tests/menu_test.cpp)
    target_include_directories(menu_test PRIVATE src)
    add_test(NAME menu_test COMMAND menu_test)
endif()
