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
execute_process(COMMAND ${LLVM_CONFIG_EXE} --libs core bitreader support
                OUTPUT_VARIABLE LLVM_LIBS_RAW
                OUTPUT_STRIP_TRAILING_WHITESPACE)

separate_arguments(LLVM_LIBS NATIVE_COMMAND "${LLVM_LIBS_RAW}")
list(TRANSFORM LLVM_LIBS REPLACE "^-l" "")

message(STATUS "Found LLVM ${LLVM_VERSION} (${LLVM_CONFIG_EXE})")

add_executable(cvm-translate translator.c)
target_include_directories(cvm-translate SYSTEM PRIVATE ${LLVM_INCDIR})
target_link_directories(cvm-translate PRIVATE ${LLVM_LIBDIR})
target_link_libraries(cvm-translate PRIVATE ${LLVM_LIBS})
target_compile_options(cvm-translate PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# --- Fixtures: compile test C sources to bitcode at build time ------------
set(_fixture_dir ${CMAKE_SOURCE_DIR}/tests/fixtures)
set(_fixture_out ${CMAKE_CURRENT_BINARY_DIR})

set(_fixture_targets "")
foreach(name add reject_double)
    set(_src ${_fixture_dir}/${name}.c)
    set(_bc  ${_fixture_out}/${name}.bc)
    add_custom_command(
        OUTPUT  ${_bc}
        COMMAND ${CMAKE_C_COMPILER} -emit-llvm -O0 -c ${_src} -o ${_bc}
        DEPENDS ${_src}
        VERBATIM
    )
    list(APPEND _fixture_targets ${_bc})
endforeach()
add_custom_target(translator_fixtures DEPENDS ${_fixture_targets})

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
