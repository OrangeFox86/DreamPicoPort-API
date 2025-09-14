cmake_minimum_required(VERSION 3.16)

project(dream_pico_port_api)

set(EXTERNAL_LIBUSB_CMAKE_DIRECTORY "ext/libusb-cmake/" CACHE STRING "Location where libusb-cmake is installed")
set(EXTERNAL_LIBUSB_LIBRARY_NAME "usb-1.0" CACHE STRING "Name of the libusb library to link against")
option(DREAMPICOPORT_API_BUILD_SHARED_LIBS "Build Shared Libraries for DreamPicoPort-API" OFF)
option(DREAMPICOPORT_TESTS "Build tests executable for DreamPicoPort" OFF)

if (DREAM_PICO_PORT_CONAN_BUILD)
    # Use conan build of libusb
    find_package(libusb REQUIRED)
    message(STATUS "libusb: ${libusb_DIR}")
    set(LIBUSB_LIBRARY_NAME "libusb::libusb")
else()
    # Build from local libusb
    option(LIBUSB_BUILD_SHARED_LIBS ${DREAMPICOPORT_API_BUILD_SHARED_LIBS})

    if(NOT "${EXTERNAL_LIBUSB_CMAKE_DIRECTORY}" STREQUAL "")
        add_subdirectory("${EXTERNAL_LIBUSB_CMAKE_DIRECTORY}")
    else()
        message(STATUS "Skipping include of cmake-libusb")
    endif()
    set(LIBUSB_LIBRARY_NAME "${EXTERNAL_LIBUSB_LIBRARY_NAME}")
endif()

add_subdirectory("src")

if (DREAMPICOPORT_TESTS)
    add_subdirectory("test")
endif()
