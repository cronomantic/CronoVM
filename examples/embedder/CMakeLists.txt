# Demonstrates the canonical consumer pattern for CronoVM: pull the VM
# in as a CMake subdirectory, build a small `.c` with cvm-cc into a
# `.bin`, and link a host program against the `cvm` library to load
# and run it. This is intentionally a stand-alone CMake project — it
# never gets configured as part of CronoVM's own build, so any
# accidentally-leaked path or assumption of being the top-level
# project surfaces immediately.

cmake_minimum_required(VERSION 3.20)
project(cvm_embedder_demo C)

# In a real downstream project this would point at wherever CronoVM
# lives (`deps/CronoVM`, a git submodule, a fetched archive, etc.).
# Here the embedder is in-tree under `examples/`, two levels deep.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. cronovm_dep)

# Compile the bytecode `.bin` from the demo `.c` source. cvm-cc is the
# user-facing one-shot driver; in a more elaborate project you'd
# probably wrap this in a function like `add_cvm_program(...)`.
set(_game_bin ${CMAKE_CURRENT_BINARY_DIR}/game.bin)
add_custom_command(
    OUTPUT  ${_game_bin}
    COMMAND $<TARGET_FILE:cvm-cc>
            ${CMAKE_CURRENT_SOURCE_DIR}/game.c
            -o ${_game_bin}
    DEPENDS cvm-cc ${CMAKE_CURRENT_SOURCE_DIR}/game.c
    VERBATIM
)
add_custom_target(embedder_game ALL DEPENDS ${_game_bin})

# Host program: links `cvm` (the static library), loads the `.bin`,
# runs it, prints the result.
add_executable(embedder_host host.c)
target_link_libraries(embedder_host PRIVATE cvm)
add_dependencies(embedder_host embedder_game)
