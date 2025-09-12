cmake_minimum_required(VERSION 3.16)

project(dream_pico_port_api)

set(LIBUSB_BUILD_SHARED_LIBS OFF)

add_subdirectory("ext/libusb-cmake/")
add_subdirectory("src")
add_subdirectory("test")

