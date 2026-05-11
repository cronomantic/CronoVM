# Toolchain file for the CronoVM RV32IMC cross-compile sanity build
# (32-bit RISC-V, integer + multiply + compressed; the embedded
# baseline — ESP32-C3, CH32V103/203, GD32VF103, BL602, etc.).
#
# Use:
#   cmake -S . -B build-rv32imc \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv32-none-elf.cmake \
#         -DCVM_NO_STDLIB_FALLBACK=ON
#   cmake --build build-rv32imc
#
# Same shape as the thumb*m toolchains. The post-build nm allowlist
# check (cmake/check_cvm_freestanding.cmake) governs both ARM and
# RISC-V builds; RISC-V brings in compiler-rt soft-float helpers
# (`__addsf3`, `__divsf3`, …) that the ARM EABI versions of GCC
# route through `__aeabi_*` names instead — both name sets are
# allowlisted.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)

# The `riscv64-unknown-elf-gcc` binary supports both rv64 and rv32 via
# -march/-mabi; package naming is historically confused (Ubuntu names
# it `gcc-riscv64-unknown-elf` even though it's the canonical bare-
# metal cross compiler for ALL RISC-V variants). Embedded MCUs use
# rv32, which we select via -march/-mabi below.
set(CMAKE_C_COMPILER   riscv64-unknown-elf-gcc)
set(CMAKE_CXX_COMPILER riscv64-unknown-elf-g++)
set(CMAKE_ASM_COMPILER riscv64-unknown-elf-gcc)
set(CMAKE_AR           riscv64-unknown-elf-ar)
set(CMAKE_RANLIB       riscv64-unknown-elf-ranlib)
set(CMAKE_OBJCOPY      riscv64-unknown-elf-objcopy)
set(CMAKE_OBJDUMP      riscv64-unknown-elf-objdump)
set(CMAKE_NM           riscv64-unknown-elf-nm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -march=rv32imc:
#   I — base 32-bit integer ISA (loads/stores/ALU/control)
#   M — integer multiply AND divide (so no __mul/div helper calls
#       at the .c level; soft-float still pulls in __mulsf3 etc.
#       because there's no F extension here)
#   C — compressed 16-bit instructions (cuts .text 20–30%)
# -mabi=ilp32: soft-float ABI (matches no F extension). Floats are
# passed and returned in integer registers via compiler-rt helpers.
#
# --specs=…/picolibc.specs adds picolibc's <math.h> etc. to the
# include search path (gcc-riscv64-unknown-elf on Ubuntu doesn't
# ship a libc, unlike gcc-arm-none-eabi which bundles newlib).
# Path is Ubuntu-specific; downstream maintainers on other distros
# can point at their own picolibc install.
set(_rv_picolibc_specs "/usr/lib/picolibc/riscv64-unknown-elf/picolibc.specs")
set(_rv_flags "-march=rv32imc -mabi=ilp32 -mcmodel=medlow -ffreestanding -fno-builtin --specs=${_rv_picolibc_specs}")
set(CMAKE_C_FLAGS_INIT   "${_rv_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_rv_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_rv_flags}")

set(CVM_EMBEDDED_SANITY_BUILD ON CACHE BOOL
    "Building only the bare-metal cross-compile sanity target" FORCE)
