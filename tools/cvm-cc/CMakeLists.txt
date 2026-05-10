# Single-command driver around clang + cvm-translate. Pure C, no LLVM
# dependency — cvm-cc just shells out to the two tools.

add_executable(cvm-cc cvm-cc.c)
target_compile_options(cvm-cc PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# Bake the runtime header dir in at build time so users can include
# `cvm_intrin.h` etc. without having to set -I themselves. The CLI
# flag --runtime-dir overrides at run time.
target_compile_definitions(cvm-cc PRIVATE
    CVM_RUNTIME_DIR="${CMAKE_SOURCE_DIR}/runtime/lib"
)

# Same trick for cvm-translate: bake in the build-tree path so cvm-cc
# from `build/tools/cvm-cc/` finds its sibling at `build/tools/translator/`
# without PATH plumbing. Only added if the translator target exists
# (the translator is conditional on llvm-config being available).
if(TARGET cvm-translate)
    target_compile_definitions(cvm-cc PRIVATE
        CVM_TRANSLATOR_DEFAULT="$<TARGET_FILE:cvm-translate>"
    )
    add_dependencies(cvm-cc cvm-translate)

    # End-to-end round trip: drive cvm-cc on a .c source, run the
    # resulting .bin under test_e2e. Catches regressions in the
    # wrapper's argv plumbing, tool discovery, and intermediate-file
    # handling. Lives here (not under tools/translator) so both the
    # cvm-cc and cvm-translate targets exist when the test is defined.
    set(_fixture_dir ${CMAKE_SOURCE_DIR}/tests/fixtures)
    set(_cvmcc_bin ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_add.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_bin}
        COMMAND $<TARGET_FILE:cvm-cc> ${_fixture_dir}/add.c -o ${_cvmcc_bin}
        DEPENDS cvm-cc cvm-translate ${_fixture_dir}/add.c
        VERBATIM
    )
    add_custom_target(cvm_cc_round_trip DEPENDS ${_cvmcc_bin})
    add_test(NAME cvm_cc_compile_add
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_round_trip)
    set_tests_properties(cvm_cc_compile_add
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_add
             COMMAND test_e2e ${_cvmcc_bin} 12 7 5)
    set_tests_properties(e2e_cvm_cc_add
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)
endif()
