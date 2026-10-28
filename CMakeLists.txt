cmake_minimum_required(VERSION 3.22)

set(PROJECT_VER "1.2.14-public1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(desktop_buddy)
# Apply after component configuration: ESP-DL adds -O3 in its own CMake file.
# Assembly inference kernels are unchanged; compact C++ leaves OTA headroom.
idf_component_get_property(buddy_dl_lib espressif__esp-dl COMPONENT_LIB)
get_target_property(buddy_dl_options ${buddy_dl_lib} COMPILE_OPTIONS)
list(REMOVE_ITEM buddy_dl_options "-O3")
set_property(TARGET ${buddy_dl_lib} PROPERTY COMPILE_OPTIONS "${buddy_dl_options}")
target_compile_options(${buddy_dl_lib} PRIVATE -Os)
