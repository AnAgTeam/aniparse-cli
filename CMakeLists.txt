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

# ---- EXTENSION SEAM (A): static composition of extra parser sets
#
# The public binary ships only the showcase parsers. To build your own anip with
# additional (e.g. private) parsers, point these at an extension library that
# exposes a registrar `void reg(aniparse::ParserStore&)` — no dynamic loading, so
# no ABI/RCE surface; the extension is compiled and linked like any other target.
#
#   cmake --preset x64-debug \
#     -D ANIP_EXTENSION_SUBDIRS="../my-extensions" \
#     -D ANIP_EXTENSION_LIBS="my-extensions" \
#     -D ANIP_EXTENSION_HEADERS="my/extensions/Register.hpp" \
#     -D ANIP_EXTENSION_REGISTRARS="myext::emplace_my_parsers"
set(ANIP_EXTENSION_SUBDIRS    "" CACHE STRING "Source dirs to add_subdirectory before linking anip")
set(ANIP_EXTENSION_LIBS       "" CACHE STRING "Extra targets to link into anip")
set(ANIP_EXTENSION_HEADERS    "" CACHE STRING "Headers declaring the extension registrar functions")
set(ANIP_EXTENSION_REGISTRARS "" CACHE STRING "Registrar functions, each void(aniparse::ParserStore&)")

# Bring extension targets into the build (unique binary dir per entry, since the
# source dirs typically live outside this tree).
set(_anip_ext_i 0)
foreach(_dir IN LISTS ANIP_EXTENSION_SUBDIRS)
	add_subdirectory("${_dir}" "${CMAKE_CURRENT_BINARY_DIR}/anip-ext-${_anip_ext_i}")
	math(EXPR _anip_ext_i "${_anip_ext_i}+1")
endforeach()

# Expand the header/registrar lists into the generated composition header.
set(ANIP_EXTENSION_INCLUDES "")
foreach(_hdr IN LISTS ANIP_EXTENSION_HEADERS)
	string(APPEND ANIP_EXTENSION_INCLUDES "#include \"${_hdr}\"\n")
endforeach()
set(ANIP_EXTENSION_CALLS "")
foreach(_reg IN LISTS ANIP_EXTENSION_REGISTRARS)
	string(APPEND ANIP_EXTENSION_CALLS "\t${_reg}(store);\n")
endforeach()
configure_file(
	"${CMAKE_CURRENT_LIST_DIR}/cmake/anip_extensions.hpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/anip_extensions.hpp"
	@ONLY)

add_executable(anip
	"src/main.cpp"
	"src/CliFilters.cpp"
	"src/CliDownload.cpp"
)

set_property(TARGET anip PROPERTY CXX_STANDARD 23)
set_property(TARGET anip PROPERTY CXX_STANDARD_REQUIRED ON)

# The generated seam header lives in the build tree.
target_include_directories(anip PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Hold the CLI to the same warning bar as the core and the parsers.
target_compile_options(anip PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/W4>
	$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

# aniparse-parsers PUBLIC-links aniparse, so linking it transitively brings the
# core target, its headers, and boost::json. Extension libs (if any) link too.
target_link_libraries(anip PRIVATE aniparse-parsers aniparse ${ANIP_EXTENSION_LIBS})
