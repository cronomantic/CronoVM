# Bytecode disassembler for CronoVM .crom/.bin images. Pure C, no LLVM
# dependency — it just decodes the CODE section and (optionally) labels
# functions / CALL targets from the `<image>.sym` sidecar a CVM_SYMS
# translate writes. Useful for inspecting translator output and debugging
# runtime faults (map a faulting pc / FUNCS slot back to source).
add_executable(cvm-dis cvm-dis.c)
target_include_directories(cvm-dis PRIVATE ${CVM_SOURCE_ROOT}/include)
target_compile_options(cvm-dis PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
