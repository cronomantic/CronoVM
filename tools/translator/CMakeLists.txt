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

add_executable(cvm-translate translator.c)
target_include_directories(cvm-translate PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(cvm-translate SYSTEM PRIVATE ${LLVM_INCDIR})
target_link_directories(cvm-translate PRIVATE ${LLVM_LIBDIR})
target_link_libraries(cvm-translate PRIVATE ${LLVM_LIBS})
target_compile_options(cvm-translate PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# --- Fixtures: compile test C sources to bitcode at build time ------------
# -O1 promotes allocas (mem2reg) which step-5 codegen requires; the
# validation tests don't care about the level, so we use -O1 everywhere.
set(_fixture_dir ${CMAKE_SOURCE_DIR}/tests/fixtures)
set(_fixture_out ${CMAKE_CURRENT_BINARY_DIR})

set(_fixture_targets "")
foreach(name add pick fib abs max divmod bitops counter sum_global
             alloc_sum two_funcs fib_recursive many_args alloca_swap
             fnptr dispatch_table narrow_ops mem_intrin reject_double)
    set(_src ${_fixture_dir}/${name}.c)
    set(_bc  ${_fixture_out}/${name}.bc)
    add_custom_command(
        OUTPUT  ${_bc}
        COMMAND ${CMAKE_C_COMPILER} --target=i386-elf -emit-llvm -O1
                                    -I${CMAKE_SOURCE_DIR}/runtime/lib
                                    -c ${_src} -o ${_bc}
        DEPENDS ${_src} ${CMAKE_SOURCE_DIR}/runtime/lib/cvm_alloc.h
        VERBATIM
    )
    list(APPEND _fixture_targets ${_bc})
endforeach()
add_custom_target(translator_fixtures DEPENDS ${_fixture_targets})

# Emit each .bin via the translator. Depends on cvm-translate + the .bc.
set(_emit_targets "")
foreach(name add pick fib abs max divmod bitops counter sum_global
             two_funcs fib_recursive many_args alloca_swap fnptr
             dispatch_table narrow_ops mem_intrin)
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
add_test(NAME e2e_two_funcs  COMMAND test_e2e ${_fixture_out}/two_funcs.bin 13 7 5)
add_test(NAME e2e_fib_rec_10 COMMAND test_e2e ${_fixture_out}/fib_recursive.bin 55 10)
add_test(NAME e2e_fib_rec_1  COMMAND test_e2e ${_fixture_out}/fib_recursive.bin  1  1)
add_test(NAME e2e_fib_rec_0   COMMAND test_e2e ${_fixture_out}/fib_recursive.bin  0  0)
add_test(NAME e2e_many_args   COMMAND test_e2e ${_fixture_out}/many_args.bin    56 10 10)
add_test(NAME e2e_alloca_swap COMMAND test_e2e ${_fixture_out}/alloca_swap.bin   3  4 7)
add_test(NAME e2e_fnptr_add        COMMAND test_e2e ${_fixture_out}/fnptr.bin           11 1 10)
add_test(NAME e2e_fnptr_sub        COMMAND test_e2e ${_fixture_out}/fnptr.bin            9 0 10)
add_test(NAME e2e_dispatch_add     COMMAND test_e2e ${_fixture_out}/dispatch_table.bin  10 0 7 3)
add_test(NAME e2e_dispatch_sub     COMMAND test_e2e ${_fixture_out}/dispatch_table.bin   4 1 7 3)
add_test(NAME e2e_dispatch_mul     COMMAND test_e2e ${_fixture_out}/dispatch_table.bin  21 2 7 3)
add_test(NAME e2e_narrow_ops       COMMAND test_e2e ${_fixture_out}/narrow_ops.bin    16214 8)
add_test(NAME e2e_mem_intrin       COMMAND test_e2e ${_fixture_out}/mem_intrin.bin     226 10)
set_tests_properties(
    e2e_add e2e_pick_true e2e_pick_false e2e_fib_10 e2e_fib_0
    e2e_abs_neg e2e_abs_pos e2e_max_lhs e2e_max_rhs
    e2e_divmod e2e_bitops e2e_counter e2e_sum_global e2e_alloc_sum
    e2e_two_funcs e2e_fib_rec_10 e2e_fib_rec_1 e2e_fib_rec_0
    e2e_many_args e2e_alloca_swap e2e_fnptr_add e2e_fnptr_sub
    e2e_dispatch_add e2e_dispatch_sub e2e_dispatch_mul
    e2e_narrow_ops e2e_mem_intrin
    PROPERTIES FIXTURES_REQUIRED emitted_bins)
