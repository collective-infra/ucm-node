# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

find_package(Git QUIET)
set(FW_GIT_HASH "no_hash")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=7
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE FW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

string(TIMESTAMP FW_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

# Re-run CMake (refreshing hash and timestamp) whenever a source file changes
file(GLOB_RECURSE FW_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_LIST_DIR}/main/*.c ${CMAKE_CURRENT_LIST_DIR}/main/*.h)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
     ${FW_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/.git/HEAD)

idf_build_set_property(COMPILE_DEFINITIONS
    "-DFW_COMMIT_HASH=\"${FW_GIT_HASH}\""
    "-DFW_BUILD_TIME=\"${FW_BUILD_TIME}\"" APPEND)

project(ucm_node)
