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

# When CVM_NO_STDLIB_FALLBACK is set (via -D on the command line),
# propagate it into the cvm target so the gate in src/cvm.c actually
# fires. Public because consumers compiling against libcvm.a need to
# see the same definition to pick the right ABI on cvm_load.
if(CVM_NO_STDLIB_FALLBACK)
    target_compile_definitions(cvm PUBLIC CVM_NO_STDLIB_FALLBACK)
endif()

# cvm.c calls sqrtf() (FSQRT opcode). GCC on Linux does not link libm
# implicitly. On Windows the C runtime covers math; on Unix-likes the
# `-lm` switch is the portable spelling (libm.so / libSystem stub).
# PUBLIC because consumers (test_e2e, examples/embedder/host, etc.)
# link libcvm.a statically and inherit its undefined references.
if(UNIX)
    target_link_libraries(cvm PUBLIC m)
endif()
# PUBLIC include path uses CVM_SOURCE_ROOT for the build interface so
# consumers that pull this in via add_subdirectory() see `cvm.h`
# regardless of where their own CMAKE_CURRENT_SOURCE_DIR points.
# The INSTALL_INTERFACE form (`include`, relative to install prefix)
# is what `find_package(CronoVM)` consumers see post-install.
target_include_directories(cvm PUBLIC
    $<BUILD_INTERFACE:${CVM_SOURCE_ROOT}/include>
    $<INSTALL_INTERFACE:include>)
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

# --- Cortex-M cross-compile sanity mode -----------------------------------
# When `cmake/toolchains/thumbv7m-none-eabi.cmake` is in use it sets
# CVM_EMBEDDED_SANITY_BUILD. In that mode the host-side test
# executables, cvm-cc / cvm-translate, the embedder/installed-consumer
# fixtures, and the install rules are all skipped — they all assume a
# hosted environment (stdio, fork+exec, llvm-config, etc.). What runs
# instead is a post-build `nm` allowlist check against libcvm.a that
# fails the build if any libc symbol leaks back in (most importantly
# malloc/free under CVM_NO_STDLIB_FALLBACK).
if(CVM_EMBEDDED_SANITY_BUILD)
    add_custom_command(TARGET cvm POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DLIBRARY=$<TARGET_FILE:cvm>
                -DNM=${CMAKE_NM}
                -DALLOWLIST=${CVM_SOURCE_ROOT}/cmake/cortex_m_allowed_symbols.txt
                -P ${CVM_SOURCE_ROOT}/cmake/check_cvm_freestanding.cmake
        COMMENT "Cortex-M sanity: checking libcvm.a undefined symbols against allowlist"
        VERBATIM)
    return()
endif()

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

add_executable(test_cvm_call tests/test_cvm_call.c)
target_link_libraries(test_cvm_call PRIVATE cvm)
target_compile_options(test_cvm_call PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
add_test(NAME cvm_call COMMAND test_cvm_call)

add_executable(test_rom tests/test_rom.c)
target_link_libraries(test_rom PRIVATE cvm)
target_compile_options(test_rom PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
add_test(NAME rom COMMAND test_rom)

add_executable(test_fnptr_export tests/test_fnptr_export.c)
target_link_libraries(test_fnptr_export PRIVATE cvm)
target_compile_options(test_fnptr_export PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
# Test registration lives in tools/translator/CMakeLists.txt where the
# fnptr_export.bin emit rule and the bc_fixtures/emitted_bins fixtures are
# defined (the translator must exist to produce the .bin).

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

# --- Install / find_package(CronoVM) ----------------------------------
#
# Default ON only when CronoVM is the top-level project being built —
# parent projects that pull CronoVM in via add_subdirectory() shouldn't
# silently inherit our install rules (they'd clash with the parent's
# own install layout). Override either way with -DCRONOVM_INSTALL=ON/OFF.
if(CMAKE_SOURCE_DIR STREQUAL CVM_SOURCE_ROOT)
    set(_cvm_install_default ON)
else()
    set(_cvm_install_default OFF)
endif()
option(CRONOVM_INSTALL "Generate install rules for CronoVM"
       ${_cvm_install_default})

if(CRONOVM_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # cvm-translate is conditional on llvm-config being available; only
    # add it to the install set if the target was actually defined.
    set(_cvm_install_targets cvm cvm-cc)
    if(TARGET cvm-translate)
        list(APPEND _cvm_install_targets cvm-translate)
    endif()

    install(TARGETS ${_cvm_install_targets}
        EXPORT   CronoVMTargets
        RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(FILES ${CVM_SOURCE_ROOT}/include/cvm.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Runtime headers (cvm_intrin.h, cvm_alloc.h) live under share/ so
    # cvm-cc's install-layout probe finds them at
    # `<bindir>/../share/cronovm/runtime/lib/`.
    install(DIRECTORY ${CVM_SOURCE_ROOT}/runtime/lib/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/cronovm/runtime/lib
        FILES_MATCHING PATTERN "*.h")

    install(EXPORT CronoVMTargets
        FILE      CronoVMTargets.cmake
        NAMESPACE cronovm::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CronoVM)

    configure_package_config_file(
        ${CVM_SOURCE_ROOT}/cmake/CronoVMConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/CronoVMConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CronoVM
        PATH_VARS CMAKE_INSTALL_DATADIR)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/CronoVMConfigVersion.cmake
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/CronoVMConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/CronoVMConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CronoVM)

    # --- ctest: install + find_package(CronoVM) round trip ------------
    # Mirrors the embedder pattern: configure + build + run a separate
    # mini-project from examples/installed_consumer/, but here the
    # consumer pulls CronoVM in via find_package against an install
    # prefix we populate at test time. Catches regressions in the
    # exported targets, the runtime-header layout, and the cvm-cc
    # install-layout probe. Skipped when the translator isn't available
    # (the consumer needs cvm-cc end-to-end, which needs cvm-translate).
    if(TARGET cvm-translate)
        set(_inst_prefix ${CMAKE_BINARY_DIR}/install_prefix)
        set(_inst_src    ${CVM_SOURCE_ROOT}/examples/installed_consumer)
        set(_inst_bin    ${CMAKE_BINARY_DIR}/installed_consumer_test)
        add_test(NAME installed_install
                 COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
                                          --prefix ${_inst_prefix})
        add_test(NAME installed_configure
                 COMMAND ${CMAKE_COMMAND} -S ${_inst_src} -B ${_inst_bin}
                                          -G ${CMAKE_GENERATOR}
                                          -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                                          -DCMAKE_PREFIX_PATH=${_inst_prefix})
        add_test(NAME installed_build
                 COMMAND ${CMAKE_COMMAND} --build ${_inst_bin})
        add_test(NAME installed_run
                 COMMAND ${_inst_bin}/installed_host ${_inst_bin}/game.bin)
        set_tests_properties(installed_install
                             PROPERTIES FIXTURES_SETUP    cvm_installed)
        set_tests_properties(installed_configure
                             PROPERTIES FIXTURES_REQUIRED cvm_installed
                                        FIXTURES_SETUP    inst_cfg)
        set_tests_properties(installed_build
                             PROPERTIES FIXTURES_REQUIRED inst_cfg
                                        FIXTURES_SETUP    inst_built)
        set_tests_properties(installed_run
                             PROPERTIES FIXTURES_REQUIRED inst_built)
    endif()
endif()
