cmake_minimum_required (VERSION 3.18)

# without this vcpkg won't try to install dependencies
if (CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg.cmake")
	message(STATUS "Found vcpkg")
	include(${CMAKE_TOOLCHAIN_FILE})
endif()

project(aniparse-cli VERSION 0.1
	DESCRIPTION "anip — a command-line interface to the libaniparse library"
	LANGUAGES CXX)

# The CLI consumes the parser library only as libraries; do not build its
# examples or test suite as part of our tree. Set before find_package so the
# dependency's option() calls honour these.
set(ANIPARSE_PARSERS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ANIPARSE_PARSERS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(aniparse-parsers REQUIRED)

add_executable(anip
	"src/main.cpp"
)

set_property(TARGET anip PROPERTY CXX_STANDARD 23)
set_property(TARGET anip PROPERTY CXX_STANDARD_REQUIRED ON)

# Hold the CLI to the same warning bar as the core and the parsers.
target_compile_options(anip PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/W4>
	$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

# aniparse-parsers PUBLIC-links aniparse, so linking it transitively brings the
# core target, its headers, and boost::json.
target_link_libraries(anip PRIVATE aniparse-parsers aniparse)
