cmake_minimum_required(VERSION 3.16)

set(CMAKE_VERBOSE_MAKEFILE ON)

file(GLOB SRC "${CMAKE_CURRENT_SOURCE_DIR}/*.c*")

add_executable(dream_pico_port_test ${SRC})
target_link_libraries(dream_pico_port_test
  PRIVATE
    dream_pico_port_api
)

target_include_directories(dream_pico_port_test
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>/../Common"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/hal/Usb>"
    "${PROJECT_SOURCE_DIR}/inc")

install(
    TARGETS
        dream_pico_port_test
    EXPORT
        dream_pico_port_api
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Development
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT Development
)
if((BUILD_SHARED_LIBS) AND (CMAKE_SYSTEM_NAME STREQUAL "Windows"))
    set_target_properties(dream_pico_port_api
        PROPERTIES
            VS_DEBUGGER_ENVIRONMENT
            "PATH=${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
    )

    install(
        FILES
            "$<TARGET_PDB_FILE:dream_pico_port_api>"
        DESTINATION
            "${CMAKE_INSTALL_BINDIR}"
        COMPONENT Development
        OPTIONAL
    )
endif()
