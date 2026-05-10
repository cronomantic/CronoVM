cmake_minimum_required(VERSION 3.20)
project(cronovm
    VERSION 0.1.0
    DESCRIPTION "Embeddable virtual machine with a Clang-driven C toolchain"
    LANGUAGES C)

# Stash CronoVM's own source root so subdirectory CMakeLists can refer
# to it portably. CMAKE_SOURCE_DIR is the top-level invocation root,
# which when CronoVM is consumed via add_subdirectory() from a parent
# project resolves to the *parent's* root, not ours — using it for
# baked-in paths (runtime headers, fixture dirs) silently breaks
# downstream consumers.
set(CVM_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL
    "Path to the CronoVM source tree (set by CronoVM's CMakeLists)")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(cvm STATIC src/cvm.c)
# PUBLIC include path uses CVM_SOURCE_ROOT so consumers that pull this
# in via add_subdirectory() see `cvm.h` regardless of where their own
# CMAKE_CURRENT_SOURCE_DIR points.
target_include_directories(cvm PUBLIC ${CVM_SOURCE_ROOT}/include)
target_compile_options(cvm PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
# Bake the version into the library so consumers can log
# `CVM_VERSION_STRING` at startup or compile-time-check with the
# numeric components.
target_compile_definitions(cvm PUBLIC
    CVM_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CVM_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CVM_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CVM_VERSION_STRING="${PROJECT_VERSION}"
)

enable_testing()
add_executable(test_vm tests/test_vm.c)
target_link_libraries(test_vm PRIVATE cvm)
target_compile_options(test_vm PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
add_test(NAME vm_basic COMMAND test_vm)

add_executable(test_e2e tests/test_e2e.c)
target_link_libraries(test_e2e PRIVATE cvm)
target_compile_options(test_e2e PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

add_executable(test_regions tests/test_regions.c)
target_link_libraries(test_regions PRIVATE cvm)
target_compile_options(test_regions PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

add_executable(test_allocator tests/test_allocator.c)
target_link_libraries(test_allocator PRIVATE cvm)
target_compile_options(test_allocator PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# --- Optional: translator (requires LLVM dev install with llvm-config) ----
add_subdirectory(tools/translator)

# --- cvm-cc: thin driver, no LLVM dependency. Always built. -----------
add_subdirectory(tools/cvm-cc)

# --- examples/embedder consumer test ----------------------------------
# Validates the canonical "add_subdirectory(deps/CronoVM)" consumer
# pattern by configuring + building + running examples/embedder/ as a
# fresh CMake project. Catches regressions where CronoVM's CMake
# implicitly assumes it's the top-level invocation (e.g. CMAKE_SOURCE_DIR
# leaks). Skipped when the translator isn't available because the
# example needs cvm-cc end-to-end.
if(TARGET cvm-translate)
    set(_emb_src ${CVM_SOURCE_ROOT}/examples/embedder)
    set(_emb_bin ${CMAKE_BINARY_DIR}/embedder_test)
    add_test(NAME embedder_configure
             COMMAND ${CMAKE_COMMAND} -S ${_emb_src} -B ${_emb_bin}
                                      -G ${CMAKE_GENERATOR}
                                      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
    add_test(NAME embedder_build
             COMMAND ${CMAKE_COMMAND} --build ${_emb_bin})
    add_test(NAME embedder_run
             COMMAND ${_emb_bin}/embedder_host ${_emb_bin}/game.bin)
    set_tests_properties(embedder_configure
                         PROPERTIES FIXTURES_SETUP    embedder_cfg)
    set_tests_properties(embedder_build
                         PROPERTIES FIXTURES_REQUIRED embedder_cfg
                                    FIXTURES_SETUP    embedder_built)
    set_tests_properties(embedder_run
                         PROPERTIES FIXTURES_REQUIRED embedder_built)
endif()
