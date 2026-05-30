# Demonstrates the installed-copy consumer pattern: pull CronoVM in via
# `find_package(CronoVM)` against an installed prefix, then build the
# same host program + game.bin as examples/embedder/. Mirrors what a
# downstream project that vendored a tagged CronoVM release into its
# own packaging system would do.
#
# To exercise manually:
#     cmake --install <cronovm-build> --prefix /tmp/cvm
#     cmake -S examples/installed_consumer -B build_inst \
#           -DCMAKE_PREFIX_PATH=/tmp/cvm
#     cmake --build build_inst
#     ./build_inst/installed_host ./build_inst/game.bin
#
# CronoVM's own ctest does this end-to-end via the `installed_*`
# tests in the parent CMakeLists.txt.

cmake_minimum_required(VERSION 3.20)
project(cvm_installed_consumer C)

find_package(CronoVM 0.2 REQUIRED)

if(NOT TARGET cronovm::cvm-translate)
    message(FATAL_ERROR
        "This example needs cvm-translate (the LLVM-bitcode translator) "
        "installed. CronoVM was likely built without llvm-config available.")
endif()

# Reuse the embedder's host + game sources verbatim. The only thing
# that changes between the two examples is HOW they get the cvm
# library (add_subdirectory vs find_package); the host code itself is
# identical.
set(_shared_src ${CMAKE_CURRENT_SOURCE_DIR}/../embedder)

set(_game_bin ${CMAKE_CURRENT_BINARY_DIR}/game.bin)
add_custom_command(
    OUTPUT  ${_game_bin}
    COMMAND $<TARGET_FILE:cronovm::cvm-cc>
            ${_shared_src}/game.c
            -o ${_game_bin}
    DEPENDS cronovm::cvm-cc ${_shared_src}/game.c
    VERBATIM
)
add_custom_target(installed_game ALL DEPENDS ${_game_bin})

add_executable(installed_host ${_shared_src}/host.c)
target_link_libraries(installed_host PRIVATE cronovm::cvm)
add_dependencies(installed_host installed_game)
