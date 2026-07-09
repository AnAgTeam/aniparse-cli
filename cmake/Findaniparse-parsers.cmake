# Source-level composition, mirroring aniparse-parsers' own Findaniparse.cmake:
# pull the parsers submodule in as a subdirectory (it in turn pulls its own
# libaniparse submodule), so the CLI builds against matched core+parser sources
# with no install step. The dependency's examples/tests are turned off by the
# top-level CMakeLists before this runs — the CLI only needs the libraries.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/aniparse-parsers" aniparse-parsers-build)

set(ANIPARSE_PARSERS_LIBRARIES aniparse-parsers aniparse)
set(ANIPARSE_PARSERS_FOUND ON)

message(STATUS "Found aniparse-parsers libraries: ${ANIPARSE_PARSERS_LIBRARIES}")
