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
    CVM_RUNTIME_DIR="${CVM_SOURCE_ROOT}/runtime/lib"
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
    set(_fixture_dir ${CVM_SOURCE_ROOT}/tests/fixtures)
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

    # Multi-file round trip: cvm-cc compiles two .c files and llvm-links them
    # into one module. The fixtures define same-named file-local statics that
    # must stay distinct — proves true per-TU linkage (no amalgamation). The
    # entry vm_main(x,y) = 2x + y + 101, so vm_main(7,5) = 120.
    set(_cvmcc_multi ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_multi.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_multi}
        COMMAND $<TARGET_FILE:cvm-cc>
                ${_fixture_dir}/multi_link_main.c
                ${_fixture_dir}/multi_link_helper.c
                -o ${_cvmcc_multi}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/multi_link_main.c
                ${_fixture_dir}/multi_link_helper.c
        VERBATIM
    )
    add_custom_target(cvm_cc_multi_round_trip DEPENDS ${_cvmcc_multi})
    add_test(NAME cvm_cc_compile_multi
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_multi_round_trip)
    set_tests_properties(cvm_cc_compile_multi
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_multi
             COMMAND test_e2e ${_cvmcc_multi} 120 7 5)
    set_tests_properties(e2e_cvm_cc_multi
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)

    # Same multi-file build with --lto (opt 'default<O2>' on the linked module,
    # vectorisation off). Guards that the cross-file optimisation path stays
    # translator-compatible and correct: vm_main(7,5) must still be 120.
    set(_cvmcc_lto ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_lto.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_lto}
        COMMAND $<TARGET_FILE:cvm-cc> --lto
                ${_fixture_dir}/multi_link_main.c
                ${_fixture_dir}/multi_link_helper.c
                -o ${_cvmcc_lto}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/multi_link_main.c
                ${_fixture_dir}/multi_link_helper.c
        VERBATIM
    )
    add_custom_target(cvm_cc_lto_round_trip DEPENDS ${_cvmcc_lto})
    add_test(NAME cvm_cc_compile_lto
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_lto_round_trip)
    set_tests_properties(cvm_cc_compile_lto
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_lto
             COMMAND test_e2e ${_cvmcc_lto} 120 7 5)
    set_tests_properties(e2e_cvm_cc_lto
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)

    # f64 legalisation end-to-end: native `double` in f64_slice.c is lowered by
    # the translator to soft-float runtime calls. The runtime (cvm_float64_rt.c)
    # is NOT listed here on purpose — cvm-cc AUTO-LINKS it after probing the
    # module for double usage. Exercises sitofp/fadd/fsub/fmul/fcmp/fptosi/
    # fptoui/fneg with caller-save spill across each call, plus the auto-link
    # path. f64_slice_main returns 0 on success for any seed.
    set(_cvmcc_f64 ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_f64_slice.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_f64}
        COMMAND $<TARGET_FILE:cvm-cc>
                ${_fixture_dir}/f64_slice.c
                -o ${_cvmcc_f64}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/f64_slice.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_float64_rt.c
        VERBATIM
    )
    add_custom_target(cvm_cc_f64_round_trip DEPENDS ${_cvmcc_f64})
    add_test(NAME cvm_cc_compile_f64
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_f64_round_trip)
    set_tests_properties(cvm_cc_compile_f64
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_f64_slice
             COMMAND test_e2e ${_cvmcc_f64} 0 0)
    set_tests_properties(e2e_cvm_cc_f64_slice
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)

    # f64 intrinsics: clang's default -ffp-contract=on contracts a*b±c into
    # llvm.fmuladd.f64 (lowered to fmul+fadd); __builtin_fabs/copysign ->
    # llvm.{fabs,copysign}.f64 (inline). f64_fma_main returns 0 for any seed.
    set(_cvmcc_fma ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_f64_fma.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_fma}
        COMMAND $<TARGET_FILE:cvm-cc>
                ${_fixture_dir}/f64_fma.c
                -o ${_cvmcc_fma}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/f64_fma.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_float64_rt.c
        VERBATIM
    )
    add_custom_target(cvm_cc_fma_round_trip DEPENDS ${_cvmcc_fma})
    add_test(NAME cvm_cc_compile_f64_fma
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_fma_round_trip)
    set_tests_properties(cvm_cc_compile_f64_fma
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_f64_fma
             COMMAND test_e2e ${_cvmcc_fma} 0 0)
    set_tests_properties(e2e_cvm_cc_f64_fma
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)

    # i64 multiply (inline MUL/MULHU) + divide/remainder (soft runtime call).
    # cvm-cc AUTO-LINKS cvm_int64_rt.c after probing the module for i64 div/rem
    # (the runtime is not listed here). f64-style sret ABI for the div helpers.
    # i64_muldiv_main returns 0 on success for any seed.
    set(_cvmcc_md ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_i64_muldiv.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_md}
        COMMAND $<TARGET_FILE:cvm-cc>
                ${_fixture_dir}/i64_muldiv.c
                -o ${_cvmcc_md}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/i64_muldiv.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_int64_rt.c
        VERBATIM
    )
    add_custom_target(cvm_cc_i64_muldiv_round_trip DEPENDS ${_cvmcc_md})
    add_test(NAME cvm_cc_compile_i64_muldiv
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_i64_muldiv_round_trip)
    set_tests_properties(cvm_cc_compile_i64_muldiv
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_i64_muldiv
             COMMAND test_e2e ${_cvmcc_md} 0 0)
    set_tests_properties(e2e_cvm_cc_i64_muldiv
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)

    # i64/f64 completeness gaps: loop-carried i64 AND f64 phis, i64/f64 select,
    # i64 variable-amount shifts (runtime), and llvm.sqrt.f64. Both runtimes are
    # auto-linked. i64f64_gaps_main returns 0 on success for any seed.
    set(_cvmcc_gaps ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_i64f64_gaps.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_gaps}
        COMMAND $<TARGET_FILE:cvm-cc>
                ${_fixture_dir}/i64f64_gaps.c
                -o ${_cvmcc_gaps}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/i64f64_gaps.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_int64_rt.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_float64_rt.c
        VERBATIM
    )
    add_custom_target(cvm_cc_gaps_round_trip DEPENDS ${_cvmcc_gaps})
    add_test(NAME cvm_cc_compile_i64f64_gaps
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_gaps_round_trip)
    set_tests_properties(cvm_cc_compile_i64f64_gaps
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_i64f64_gaps
             COMMAND test_e2e ${_cvmcc_gaps} 0 0)
    set_tests_properties(e2e_cvm_cc_i64f64_gaps
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)

    # 64-bit calling convention (phase 3): i64/f64 as function args and return
    # values across real (noinline) calls — incl. mixed scalar/wide args and
    # five-i64 args that straddle to the stack. Runtimes auto-linked as needed.
    # abi64_main returns 0 on success for any seed.
    set(_cvmcc_abi ${CMAKE_CURRENT_BINARY_DIR}/cvm_cc_abi64.bin)
    add_custom_command(
        OUTPUT  ${_cvmcc_abi}
        COMMAND $<TARGET_FILE:cvm-cc>
                ${_fixture_dir}/abi64.c
                -o ${_cvmcc_abi}
        DEPENDS cvm-cc cvm-translate
                ${_fixture_dir}/abi64.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_int64_rt.c
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_float64_rt.c
        VERBATIM
    )
    add_custom_target(cvm_cc_abi64_round_trip DEPENDS ${_cvmcc_abi})
    add_test(NAME cvm_cc_compile_abi64
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                      --target cvm_cc_abi64_round_trip)
    set_tests_properties(cvm_cc_compile_abi64
                         PROPERTIES FIXTURES_SETUP cvm_cc_bins)
    add_test(NAME e2e_cvm_cc_abi64
             COMMAND test_e2e ${_cvmcc_abi} 0 0)
    set_tests_properties(e2e_cvm_cc_abi64
                         PROPERTIES FIXTURES_REQUIRED cvm_cc_bins)
endif()
