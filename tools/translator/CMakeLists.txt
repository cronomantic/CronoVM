# LLVM is queried via llvm-config (the find_package(LLVM CONFIG) path
# transitively requires zlib/libxml2/libffi imported targets that aren't
# discoverable on every install — llvm-config is the supported alternative).

find_program(LLVM_CONFIG_EXE llvm-config DOC "Path to llvm-config")
if(NOT LLVM_CONFIG_EXE)
    message(STATUS "llvm-config not found — skipping translator")
    return()
endif()

execute_process(COMMAND ${LLVM_CONFIG_EXE} --version
                OUTPUT_VARIABLE LLVM_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXE} --includedir
                OUTPUT_VARIABLE LLVM_INCDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXE} --libdir
                OUTPUT_VARIABLE LLVM_LIBDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${LLVM_CONFIG_EXE} --libs core bitreader support target
                OUTPUT_VARIABLE LLVM_LIBS_RAW
                OUTPUT_STRIP_TRAILING_WHITESPACE)

separate_arguments(LLVM_LIBS NATIVE_COMMAND "${LLVM_LIBS_RAW}")
list(TRANSFORM LLVM_LIBS REPLACE "^-l" "")

message(STATUS "Found LLVM ${LLVM_VERSION} (${LLVM_CONFIG_EXE})")

# C++ is needed only for llvm_c_compat.cpp — a thin shim around the
# LLVM C++ API for SwitchInst case-value retrieval. The LLVM-C wrapper
# (`LLVMGetSwitchCaseValue`) was only added very recently in upstream
# llvm-c/Core.h (LLVM 20+); older but otherwise-fine toolchains
# (Ubuntu 24.04's llvm-19, llvm-18) lack the symbol but still ship
# the C++ headers, so the shim gets us back to portable territory.
enable_language(CXX)

add_executable(cvm-translate translator.c llvm_c_compat.cpp)
target_include_directories(cvm-translate PRIVATE ${CVM_SOURCE_ROOT}/include)
target_include_directories(cvm-translate SYSTEM PRIVATE ${LLVM_INCDIR})
target_link_directories(cvm-translate PRIVATE ${LLVM_LIBDIR})
target_link_libraries(cvm-translate PRIVATE ${LLVM_LIBS})
target_compile_features(cvm-translate PRIVATE cxx_std_17)
target_compile_options(cvm-translate PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# --- Bitcode-parser fuzz harness (optional) -------------------------------
# `cvm_fuzz_translate_buffer` (in translator.c) drives a parse + codegen
# pipeline on an in-memory bitcode blob without writing anything to disk.
# It is the unit-of-work for the libFuzzer harness in fuzz_translate.c.
#
# Two targets, both gated on CVM_BUILD_FUZZER (default OFF):
#
#   * cvm-translate-fuzz       — real libFuzzer build, ASAN+UBSAN, only
#                                offered when the compiler actually
#                                supports `-fsanitize=fuzzer` (clang on
#                                Linux/macOS; mingw clang does NOT).
#   * cvm-translate-fuzz-replay — standalone driver, no instrumentation.
#                                Replays a corpus directory one file at
#                                a time. Useful on hosts without libFuzzer
#                                and as a smoke-test for the harness
#                                wiring even on libFuzzer-capable hosts.
#
# Both link against translator.c built with -DCVM_NO_TRANSLATOR_MAIN so
# the CLI's `main()` doesn't collide with libFuzzer's (or the replay
# driver's) own `main()`.
option(CVM_BUILD_FUZZER "Build translator bitcode-parser fuzz harness" OFF)
if(CVM_BUILD_FUZZER)
    include(CheckCSourceCompiles)
    # Probe in static-library scope so CheckCSourceCompiles doesn't try
    # to link an executable — the probe code defines LLVMFuzzerTestOneInput
    # only (no main), and we want to test the compile flag, not the
    # final-link plumbing.
    set(_save_target_type ${CMAKE_TRY_COMPILE_TARGET_TYPE})
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer-no-link")
    check_c_source_compiles(
        "int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long s){(void)d;(void)s;return 0;}"
        CVM_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE ${_save_target_type})

    if(CVM_HAVE_LIBFUZZER)
        add_executable(cvm-translate-fuzz
            translator.c llvm_c_compat.cpp fuzz_translate.c)
        target_include_directories(cvm-translate-fuzz PRIVATE ${CVM_SOURCE_ROOT}/include)
        target_include_directories(cvm-translate-fuzz SYSTEM PRIVATE ${LLVM_INCDIR})
        target_link_directories(cvm-translate-fuzz PRIVATE ${LLVM_LIBDIR})
        target_link_libraries(cvm-translate-fuzz PRIVATE ${LLVM_LIBS})
        target_compile_features(cvm-translate-fuzz PRIVATE cxx_std_17)
        target_compile_definitions(cvm-translate-fuzz PRIVATE
            CVM_NO_TRANSLATOR_MAIN)
        target_compile_options(cvm-translate-fuzz PRIVATE
            -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
        target_link_options(cvm-translate-fuzz PRIVATE
            -fsanitize=fuzzer,address,undefined)
        message(STATUS "Fuzz: libFuzzer harness enabled (cvm-translate-fuzz)")
    else()
        message(STATUS "Fuzz: compiler does not support -fsanitize=fuzzer "
                       "— skipping cvm-translate-fuzz")
    endif()

    # Standalone replay driver — no sanitizers, works everywhere.
    add_executable(cvm-translate-fuzz-replay
        translator.c llvm_c_compat.cpp fuzz_translate.c)
    target_include_directories(cvm-translate-fuzz-replay PRIVATE ${CVM_SOURCE_ROOT}/include)
    target_include_directories(cvm-translate-fuzz-replay SYSTEM PRIVATE ${LLVM_INCDIR})
    target_link_directories(cvm-translate-fuzz-replay PRIVATE ${LLVM_LIBDIR})
    target_link_libraries(cvm-translate-fuzz-replay PRIVATE ${LLVM_LIBS})
    target_compile_features(cvm-translate-fuzz-replay PRIVATE cxx_std_17)
    target_compile_definitions(cvm-translate-fuzz-replay PRIVATE
        CVM_NO_TRANSLATOR_MAIN CVM_FUZZER_STANDALONE)
endif()

# --- Fixtures: compile test C sources to bitcode at build time ------------
# -O1 promotes allocas (mem2reg) which step-5 codegen requires; the
# validation tests don't care about the level, so we use -O1 everywhere.
#
# Fixture compilation needs clang specifically (`--target=i386-elf
# -emit-llvm`). On hosts where CMAKE_C_COMPILER is GCC or MSVC,
# CMAKE_C_COMPILER cannot drive it — so we resolve a separate clang
# binary here. Fall back to CMAKE_C_COMPILER when no standalone clang
# is on the PATH (preserves the in-tree dev flow where clang is the
# C compiler).
find_program(CVM_FIXTURE_CC NAMES clang)
if(NOT CVM_FIXTURE_CC)
    set(CVM_FIXTURE_CC ${CMAKE_C_COMPILER})
    message(STATUS "Fixture compiler: ${CVM_FIXTURE_CC} (CMAKE_C_COMPILER fallback)")
else()
    message(STATUS "Fixture compiler: ${CVM_FIXTURE_CC}")
endif()

set(_fixture_dir ${CVM_SOURCE_ROOT}/tests/fixtures)
set(_fixture_out ${CMAKE_CURRENT_BINARY_DIR})

set(_fixture_targets "")
foreach(name add pick fib abs max divmod bitops counter sum_global
             alloc_sum free_list regions mulh float_basic fsqrt
             i64_basic f64_basic two_funcs
             fib_recursive many_args alloca_swap fnptr fnptr_export
             dispatch_table const_gep reg_pressure
             narrow_ops mem_intrin long_branch switch_dispatch
             switch_table switch_neg_table spill_loop reject_double)
    set(_src ${_fixture_dir}/${name}.c)
    set(_bc  ${_fixture_out}/${name}.bc)
    # -ffreestanding turns off __STDC_HOSTED__, so clang's bundled
    # <stdint.h> stops chaining via #include_next into the system's
    # arch-specific glibc headers. Without it, `--target=i386-elf` on
    # Ubuntu reaches /usr/include/stdint.h, which needs
    # /usr/include/i386-linux-gnu/bits/libc-header-start.h — not
    # installed without libc6-dev-i386. We are bare-metal anyway, so
    # freestanding is the right semantic too.
    add_custom_command(
        OUTPUT  ${_bc}
        COMMAND ${CVM_FIXTURE_CC} --target=i386-elf -ffreestanding
                                    -emit-llvm -O1
                                    -I${CVM_SOURCE_ROOT}/runtime/lib
                                    -c ${_src} -o ${_bc}
        DEPENDS ${_src}
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_alloc.h
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_intrin.h
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_int64.h
                ${CVM_SOURCE_ROOT}/runtime/lib/cvm_float64.h
        VERBATIM
    )
    list(APPEND _fixture_targets ${_bc})
endforeach()
add_custom_target(translator_fixtures DEPENDS ${_fixture_targets})

# Stage the fixture .bc files as a fuzz seed corpus. Gated on
# CVM_BUILD_FUZZER (only useful when the fuzz targets exist).
# libFuzzer consumes seeds with `cvm-translate-fuzz <corpus_dir>`;
# the replay driver takes the same files as positional argv.
# Placed after _fixture_targets is populated so the foreach iterates.
if(CVM_BUILD_FUZZER)
    set(_corpus_dir ${CMAKE_CURRENT_BINARY_DIR}/fuzz_corpus)
    set(_corpus_stamps "")
    foreach(_bc ${_fixture_targets})
        get_filename_component(_bc_name ${_bc} NAME)
        set(_dst ${_corpus_dir}/${_bc_name})
        add_custom_command(
            OUTPUT  ${_dst}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_corpus_dir}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_bc} ${_dst}
            DEPENDS ${_bc}
            VERBATIM)
        list(APPEND _corpus_stamps ${_dst})
    endforeach()
    add_custom_target(cvm-translate-fuzz-corpus DEPENDS ${_corpus_stamps})
endif()

# Emit each .bin via the translator. Depends on cvm-translate + the .bc.
set(_emit_targets "")
foreach(name add pick fib abs max divmod bitops counter sum_global
             mulh float_basic fsqrt i64_basic f64_basic
             two_funcs fib_recursive many_args
             alloca_swap fnptr fnptr_export dispatch_table const_gep reg_pressure
             narrow_ops mem_intrin
             long_branch switch_dispatch switch_table switch_neg_table
             spill_loop)
    set(_bc  ${_fixture_out}/${name}.bc)
    set(_bin ${_fixture_out}/${name}.bin)
    add_custom_command(
        OUTPUT  ${_bin}
        COMMAND $<TARGET_FILE:cvm-translate> ${_bc} -o ${_bin}
        DEPENDS cvm-translate ${_bc}
        VERBATIM
    )
    list(APPEND _emit_targets ${_bin})
endforeach()

# alloc_sum needs a heap reserve.
add_custom_command(
    OUTPUT  ${_fixture_out}/alloc_sum.bin
    COMMAND $<TARGET_FILE:cvm-translate> ${_fixture_out}/alloc_sum.bc
                                          --heap-reserve=4K
                                          -o ${_fixture_out}/alloc_sum.bin
    DEPENDS cvm-translate ${_fixture_out}/alloc_sum.bc
    VERBATIM
)
list(APPEND _emit_targets ${_fixture_out}/alloc_sum.bin)

# free_list pins the tail of a 512-byte heap on purpose to force coalescing
# to be the only path that satisfies the final allocation.
add_custom_command(
    OUTPUT  ${_fixture_out}/free_list.bin
    COMMAND $<TARGET_FILE:cvm-translate> ${_fixture_out}/free_list.bc
                                          --heap-reserve=512
                                          -o ${_fixture_out}/free_list.bin
    DEPENDS cvm-translate ${_fixture_out}/free_list.bc
    VERBATIM
)
list(APPEND _emit_targets ${_fixture_out}/free_list.bin)

# regions: an "input" region (4 B, host writes) and a "fb" region (16 B, VM
# writes). The host harness pre-fills input, runs, and verifies fb.
add_custom_command(
    OUTPUT  ${_fixture_out}/regions.bin
    COMMAND $<TARGET_FILE:cvm-translate> ${_fixture_out}/regions.bc
                                          --region=input:4:r
                                          --region=fb:16:w
                                          -o ${_fixture_out}/regions.bin
    DEPENDS cvm-translate ${_fixture_out}/regions.bc
    VERBATIM
)
list(APPEND _emit_targets ${_fixture_out}/regions.bin)
add_custom_target(translator_emit_fixtures DEPENDS ${_emit_targets})

add_test(NAME translator_build_fixtures
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                  --target translator_fixtures)
set_tests_properties(translator_build_fixtures
                     PROPERTIES FIXTURES_SETUP bc_fixtures)

add_test(NAME translator_parse_add
         COMMAND $<TARGET_FILE:cvm-translate> ${_fixture_out}/add.bc)
set_tests_properties(translator_parse_add
                     PROPERTIES FIXTURES_REQUIRED bc_fixtures)

add_test(NAME translator_reject_double
         COMMAND $<TARGET_FILE:cvm-translate> ${_fixture_out}/reject_double.bc)
set_tests_properties(translator_reject_double PROPERTIES
                     FIXTURES_REQUIRED bc_fixtures
                     WILL_FAIL TRUE)

add_test(NAME translator_emit_fixtures
         COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                                  --target translator_emit_fixtures)
set_tests_properties(translator_emit_fixtures PROPERTIES
                     FIXTURES_SETUP emitted_bins
                     FIXTURES_REQUIRED bc_fixtures)

add_test(NAME e2e_add        COMMAND test_e2e ${_fixture_out}/add.bin     12  7 5)
add_test(NAME e2e_pick_true  COMMAND test_e2e ${_fixture_out}/pick.bin     7  1 7 9)
add_test(NAME e2e_pick_false COMMAND test_e2e ${_fixture_out}/pick.bin     9  0 7 9)
add_test(NAME e2e_fib_10     COMMAND test_e2e ${_fixture_out}/fib.bin     55  10)
add_test(NAME e2e_fib_0      COMMAND test_e2e ${_fixture_out}/fib.bin      0  0)
add_test(NAME e2e_abs_neg    COMMAND test_e2e ${_fixture_out}/abs.bin      7  -7)
add_test(NAME e2e_abs_pos    COMMAND test_e2e ${_fixture_out}/abs.bin     42  42)
add_test(NAME e2e_max_lhs    COMMAND test_e2e ${_fixture_out}/max.bin      7  7 3)
add_test(NAME e2e_max_rhs    COMMAND test_e2e ${_fixture_out}/max.bin      7  3 7)
add_test(NAME e2e_divmod     COMMAND test_e2e ${_fixture_out}/divmod.bin 302  17 5)
add_test(NAME e2e_bitops     COMMAND test_e2e ${_fixture_out}/bitops.bin 127  7 3)
add_test(NAME e2e_counter    COMMAND test_e2e ${_fixture_out}/counter.bin 7  7)
add_test(NAME e2e_sum_global COMMAND test_e2e ${_fixture_out}/sum_global.bin 55 10)
add_test(NAME e2e_alloc_sum  COMMAND test_e2e ${_fixture_out}/alloc_sum.bin 15 5)
add_test(NAME e2e_free_list  COMMAND test_e2e ${_fixture_out}/free_list.bin  0 0)
add_test(NAME e2e_regions    COMMAND test_regions ${_fixture_out}/regions.bin)
add_test(NAME e2e_allocator  COMMAND test_allocator ${_fixture_out}/add.bin)
add_test(NAME e2e_mulh       COMMAND test_e2e ${_fixture_out}/mulh.bin  0 0)
add_test(NAME e2e_float_basic COMMAND test_e2e ${_fixture_out}/float_basic.bin 0 0)
add_test(NAME e2e_fsqrt       COMMAND test_e2e ${_fixture_out}/fsqrt.bin       0 0)
add_test(NAME e2e_i64_basic   COMMAND test_e2e ${_fixture_out}/i64_basic.bin   0 0)
add_test(NAME e2e_f64_basic   COMMAND test_e2e ${_fixture_out}/f64_basic.bin   0 0)
# (cvm-cc's end-to-end test is registered in tools/cvm-cc/CMakeLists.txt
# so the cvm-cc target exists when the test is being defined.)
add_test(NAME e2e_two_funcs  COMMAND test_e2e ${_fixture_out}/two_funcs.bin 13 7 5)
add_test(NAME e2e_fib_rec_10 COMMAND test_e2e ${_fixture_out}/fib_recursive.bin 55 10)
add_test(NAME e2e_fib_rec_1  COMMAND test_e2e ${_fixture_out}/fib_recursive.bin  1  1)
add_test(NAME e2e_fib_rec_0   COMMAND test_e2e ${_fixture_out}/fib_recursive.bin  0  0)
add_test(NAME e2e_many_args   COMMAND test_e2e ${_fixture_out}/many_args.bin    56 10 10)
add_test(NAME e2e_alloca_swap COMMAND test_e2e ${_fixture_out}/alloca_swap.bin   3  4 7)
add_test(NAME e2e_fnptr_add        COMMAND test_e2e ${_fixture_out}/fnptr.bin           11 1 10)
add_test(NAME e2e_fnptr_sub        COMMAND test_e2e ${_fixture_out}/fnptr.bin            9 0 10)
# fnptr_export: a function whose address is taken and handed to the host via
# a syscall, but never internally CALLed. Guards the FUNCS-emission fix —
# the host must be able to cvm_call the registered index. Uses its own
# harness (test_fnptr_export) rather than test_e2e.
add_test(NAME e2e_fnptr_export     COMMAND test_fnptr_export ${_fixture_out}/fnptr_export.bin)
# const_gep: stores to fixed indices of an external global array, which clang
# lowers to ConstantExpr getelementptr pointer operands. Guards cg_reg_for's
# constant-GEP handling. vm_main(n) returns 2*n + 3.
add_test(NAME e2e_const_gep        COMMAND test_e2e ${_fixture_out}/const_gep.bin 23 10)
# reg_pressure: 120 constant-addressed stores to a global in one function.
# Guards the per-instruction recycling of transient (constant/global/GEP)
# registers — without it next_reg grew monotonically and overflowed the
# register file. vm_main(n) returns n + (n+60) + (n+119); n=5 -> 194.
add_test(NAME e2e_reg_pressure     COMMAND test_e2e ${_fixture_out}/reg_pressure.bin 194 5)
add_test(NAME e2e_dispatch_add     COMMAND test_e2e ${_fixture_out}/dispatch_table.bin  10 0 7 3)
add_test(NAME e2e_dispatch_sub     COMMAND test_e2e ${_fixture_out}/dispatch_table.bin   4 1 7 3)
add_test(NAME e2e_dispatch_mul     COMMAND test_e2e ${_fixture_out}/dispatch_table.bin  21 2 7 3)
add_test(NAME e2e_narrow_ops       COMMAND test_e2e ${_fixture_out}/narrow_ops.bin    16214 8)
add_test(NAME e2e_mem_intrin       COMMAND test_e2e ${_fixture_out}/mem_intrin.bin     226 10)
# long_branch: forward goto over a huge volatile-arithmetic body. The BNE
# in entry must reach `end_short` (last block) — ~145 instructions away,
# well past the ±127 imm8 reach. Relaxation MUST trigger for this binary
# to translate at all; without it the translator errors with "branch
# offset 145 out of range". Returned values captured from the working
# build (x=0 → end_short returns 999; x=7 → fall-through returns the
# final volatile-arithmetic accumulator).
add_test(NAME e2e_long_branch_zero COMMAND test_e2e ${_fixture_out}/long_branch.bin     999 0)
add_test(NAME e2e_long_branch_path COMMAND test_e2e ${_fixture_out}/long_branch.bin 1045158 7)
# switch_dispatch: every case + default + an in-range gap value. Each
# arm computes a different binary op on (x, y); we pass distinct values
# so the test result identifies which arm ran.
#                                                                expected op  x  y
add_test(NAME e2e_switch_case_0    COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin  10  0  7  3)  # x+y
add_test(NAME e2e_switch_case_1    COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin   4  1  7  3)  # x-y
add_test(NAME e2e_switch_case_2    COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin  21  2  7  3)  # x*y
add_test(NAME e2e_switch_case_5    COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin   4  5  7  3)  # x^y
add_test(NAME e2e_switch_case_10   COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin   3 10  7  3)  # x&y
add_test(NAME e2e_switch_default   COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin  -1 99  7  3)  # default
add_test(NAME e2e_switch_gap       COMMAND test_e2e ${_fixture_out}/switch_dispatch.bin  -1  3  7  3)  # gap → default
# switch_table: dense 8-case switch that triggers the jump-table form
# (n_cases=8, range=8, density=1.0). Each case picks a different binary
# op on (x=12, y=3); expected values verify which arm ran.
#                                                                expected op  x   y
add_test(NAME e2e_switch_tab_add   COMMAND test_e2e ${_fixture_out}/switch_table.bin   15  0  12  3)  # x+y
add_test(NAME e2e_switch_tab_sub   COMMAND test_e2e ${_fixture_out}/switch_table.bin    9  1  12  3)  # x-y
add_test(NAME e2e_switch_tab_mul   COMMAND test_e2e ${_fixture_out}/switch_table.bin   36  2  12  3)  # x*y
add_test(NAME e2e_switch_tab_shl   COMMAND test_e2e ${_fixture_out}/switch_table.bin   96  3  12  3)  # x<<y
add_test(NAME e2e_switch_tab_or    COMMAND test_e2e ${_fixture_out}/switch_table.bin   15  4  12  3)  # x|y
add_test(NAME e2e_switch_tab_and   COMMAND test_e2e ${_fixture_out}/switch_table.bin    0  5  12  3)  # x&y
add_test(NAME e2e_switch_tab_xor   COMMAND test_e2e ${_fixture_out}/switch_table.bin   15  6  12  3)  # x^y
add_test(NAME e2e_switch_tab_shr   COMMAND test_e2e ${_fixture_out}/switch_table.bin    1  7  12  3)  # x>>y
add_test(NAME e2e_switch_tab_def   COMMAND test_e2e ${_fixture_out}/switch_table.bin   -1 99  12  3)  # default
# switch_neg_table: dense 4-case switch with negative i8 case constants
# (-4..-1). Exercises the table form's case-constant width handling — see
# the fixture header for the bug class this guards against.
#                                                                expected op  x  y
add_test(NAME e2e_switch_neg_m4    COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin 10 -4 7 3)  # x+y
add_test(NAME e2e_switch_neg_m3    COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin  4 -3 7 3)  # x-y
add_test(NAME e2e_switch_neg_m2    COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin 21 -2 7 3)  # x*y
add_test(NAME e2e_switch_neg_m1    COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin  4 -1 7 3)  # x^y
add_test(NAME e2e_switch_neg_zero  COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin -1  0 7 3)  # default
add_test(NAME e2e_switch_neg_m5    COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin -1 -5 7 3)  # default (below lo)
add_test(NAME e2e_switch_neg_pos   COMMAND test_e2e ${_fixture_out}/switch_neg_table.bin -1  5 7 3)  # default (above hi)
# spill_loop: adversarial call-heavy loop. 12 noinline calls per iteration
# with 8 long-lived carriers all live across every call exercises spill
# compaction (ever_spilled + slot_of[]) at scale, and the resulting body
# (~1000+ emitted instructions in vm_main) forces branch relaxation on the
# back-edge BEQ/BNE. Expected returns captured from a working build; both
# match a `gcc -O1 -fwrapv` reference compile.
add_test(NAME e2e_spill_loop_0     COMMAND test_e2e ${_fixture_out}/spill_loop.bin            0 0)
add_test(NAME e2e_spill_loop_1     COMMAND test_e2e ${_fixture_out}/spill_loop.bin   -500660212 1)
add_test(NAME e2e_spill_loop_4     COMMAND test_e2e ${_fixture_out}/spill_loop.bin  -1450686863 4)
add_test(NAME e2e_spill_loop_8     COMMAND test_e2e ${_fixture_out}/spill_loop.bin   1380580815 8)
set_tests_properties(
    e2e_add e2e_pick_true e2e_pick_false e2e_fib_10 e2e_fib_0
    e2e_abs_neg e2e_abs_pos e2e_max_lhs e2e_max_rhs
    e2e_divmod e2e_bitops e2e_counter e2e_sum_global e2e_alloc_sum
    e2e_free_list e2e_regions e2e_allocator e2e_mulh e2e_float_basic
    e2e_fsqrt e2e_i64_basic e2e_f64_basic
    e2e_two_funcs e2e_fib_rec_10 e2e_fib_rec_1 e2e_fib_rec_0
    e2e_many_args e2e_alloca_swap e2e_fnptr_add e2e_fnptr_sub e2e_fnptr_export
    e2e_const_gep e2e_reg_pressure
    e2e_dispatch_add e2e_dispatch_sub e2e_dispatch_mul
    e2e_narrow_ops e2e_mem_intrin
    e2e_long_branch_zero e2e_long_branch_path
    e2e_switch_case_0 e2e_switch_case_1 e2e_switch_case_2
    e2e_switch_case_5 e2e_switch_case_10
    e2e_switch_default e2e_switch_gap
    e2e_switch_tab_add e2e_switch_tab_sub e2e_switch_tab_mul
    e2e_switch_tab_shl e2e_switch_tab_or  e2e_switch_tab_and
    e2e_switch_tab_xor e2e_switch_tab_shr e2e_switch_tab_def
    e2e_switch_neg_m4 e2e_switch_neg_m3 e2e_switch_neg_m2 e2e_switch_neg_m1
    e2e_switch_neg_zero e2e_switch_neg_m5 e2e_switch_neg_pos
    e2e_spill_loop_0 e2e_spill_loop_1 e2e_spill_loop_4 e2e_spill_loop_8
    PROPERTIES FIXTURES_REQUIRED emitted_bins)
