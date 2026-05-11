# Toolchain file for the CronoVM Cortex-M0 / M0+ / RP2040 cross-compile
# sanity build (Cortex-M0-class, ARMv6-M / thumbv6m).
#
# Same shape as `thumbv7m-none-eabi.cmake` — only the `-mcpu` flag
# differs. M0 is the lowest-common-denominator core in the family:
# no 32x32→64 multiply, no integer divide instruction, no barrel
# shifter on shifts > 1, no IT blocks. The compiler emits libgcc
# helper calls for the operations the CPU can't do in a single
# instruction, all under the `__aeabi_*` prefix the allowlist
# already permits via wildcard. The .text grows compared to M3
# (rough estimate +10–30%) but stays well under the 16 KiB target;
# the post-build nm check fails the build on actual libc leakage.
#
# Usage:
#   cmake -S . -B build-armv6m \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/thumbv6m-none-eabi.cmake \
#         -DCVM_NO_STDLIB_FALLBACK=ON
#   cmake --build build-armv6m

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR           arm-none-eabi-ar)
set(CMAKE_RANLIB       arm-none-eabi-ranlib)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP      arm-none-eabi-objdump)
set(CMAKE_NM           arm-none-eabi-nm)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mcpu=cortex-m0 is the lowest-common-denominator thumbv6m core. The
# same .a runs on M0+ (Cortex-M0+, marginally more efficient) and on
# RP2040's two M0+ cores — armv6-m is forward-compatible across the
# family. -mfloat-abi=soft is mandatory: no thumbv6m chip has an FPU.
set(_cm_flags "-mcpu=cortex-m0 -mthumb -mfloat-abi=soft -ffreestanding -fno-builtin")
set(CMAKE_C_FLAGS_INIT   "${_cm_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_cm_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_cm_flags}")

set(CVM_EMBEDDED_SANITY_BUILD ON CACHE BOOL
    "Building only the bare-metal cross-compile sanity target" FORCE)
