cmake_minimum_required(VERSION 3.16)

set(CMAKE_VERBOSE_MAKEFILE ON)

file(GLOB SRC "${CMAKE_CURRENT_SOURCE_DIR}/*.c*")

add_library(dream_pico_port_api STATIC ${SRC})
target_link_libraries(dream_pico_port_api
  PRIVATE
    usb-1.0
)

# For the most part, this lib is C++11 compatible, but std::make_unique() is used which is C++14
# I'm choosing C++17 just because C++14 was less adopted so may as well go to the next notch
set_target_properties(dream_pico_port_api PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(dream_pico_port_api
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
