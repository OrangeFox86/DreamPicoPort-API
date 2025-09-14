cmake_minimum_required(VERSION 3.16)

project(dream_pico_port_api)

set(DREAMPICOPORT_LIBUSB_CMAKE_DIRECTORY "ext/libusb-cmake/" CACHE STRING "Location where libusb-cmake is installed or leave empty if this is included by a parent CMakeLists.txt file")
option(DREAMPICOPORT_API_BUILD_SHARED_LIBS "Build Shared Libraries for DreamPicoPort-API" OFF)
option(DREAMPICOPORT_TESTS "Build tests executable for DreamPicoPort" OFF)

if (DREAM_PICO_PORT_CONAN_BUILD)
    # Use conan build of libusb
    set(DREAMPICOPORT_LIBUSB_LIBRARY_NAME "libusb::libusb")
    find_package(libusb REQUIRED)
    message(STATUS "libusb: ${libusb_DIR}")
else()
    # Build from a local libusb
    set(DREAMPICOPORT_LIBUSB_LIBRARY_NAME "usb-1.0" CACHE STRING "Name of the libusb library to link against")
    option(LIBUSB_BUILD_SHARED_LIBS ${DREAMPICOPORT_API_BUILD_SHARED_LIBS})

    if(NOT "${DREAMPICOPORT_LIBUSB_CMAKE_DIRECTORY}" STREQUAL "")
        add_subdirectory("${DREAMPICOPORT_LIBUSB_CMAKE_DIRECTORY}")
    else()
        message(STATUS "Skipping include of cmake-libusb")
    endif()
endif()

add_subdirectory("src")

if (DREAMPICOPORT_TESTS)
    add_subdirectory("test")
endif()
