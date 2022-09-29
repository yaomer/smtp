cmake_minimum_required (VERSION 2.8)

project (smtp-server)

set (CMAKE_CXX_FLAGS "-std=c++17 -Wall -O2")

set (SMTP "${PROJECT_SOURCE_DIR}/src")

set (SRC_FILES
    ${SMTP}/server.cc
    ${SMTP}/relay.cc
    ${SMTP}/config.cc
    ${SMTP}/main.cc
)

add_library(angel STATIC IMPORTED)
set_target_properties(angel PROPERTIES IMPORTED_LOCATION "/usr/local/lib/libangel.a")

add_executable(smtp-server ${SRC_FILES})
target_link_libraries(smtp-server angel)
