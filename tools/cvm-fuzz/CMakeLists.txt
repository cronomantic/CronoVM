# cvm-fuzz — UB-free differential test generator for the CronoVM translator/VM.
# Pure C, no LLVM dependency. Emits self-contained C programs (each exporting
# `int conf_main(void)` returning an int32 checksum) that the differential corpus
# runner (tests/corpus/run_corpus.sh) compiles natively + on the VM and compares,
# to catch translator/VM miscompiles proactively instead of via a game OOB.
add_executable(cvm-fuzz cvm-fuzz.c)
target_include_directories(cvm-fuzz PRIVATE ${CVM_SOURCE_ROOT}/include)
target_compile_options(cvm-fuzz PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
