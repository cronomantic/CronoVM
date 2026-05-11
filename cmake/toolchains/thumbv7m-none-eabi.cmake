# Toolchain file for the CronoVM Cortex-M cross-compile sanity build.
#
# Goal: confirm that src/cvm.c compiles for thumbv7m-none-eabi (Cortex-M3
# / M4 / M7-class) and that libcvm.a does NOT pull in any libc symbols
# the embedded host can't be expected to provide — most importantly,
# malloc/free when -DCVM_NO_STDLIB_FALLBACK is in effect.
#
# Usage:
#   cmake -S . -B build-armv7m \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/thumbv7m-none-eabi.cmake \
#         -DCVM_NO_STDLIB_FALLBACK=ON
#   cmake --build build-armv7m
#
# A post-build step (added in the top-level CMakeLists.txt when
# CVM_EMBEDDED_SANITY_BUILD is set) runs `arm-none-eabi-nm` against the
# resulting libcvm.a and fails the build if any undefined symbol falls
# outside `cmake/cortex_m_allowed_symbols.txt`.
#
# Why thumbv7m and not thumbv6m (M0/M0+)? thumbv7m covers the bulk of
# realistic deployment targets (M3, M4F, M7) with one toolchain file.
# M0-class chips need different compiler flags AND will surface
# additional libgcc soft-arithmetic helpers (no 32-bit multiply
# instruction). A dedicated `thumbv6m-none-eabi.cmake` follow-up
# toolchain is the right shape if/when an audit asks for M0 coverage.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# arm-none-eabi-gcc is widely packaged (apt: gcc-arm-none-eabi; brew:
# arm-none-eabi-gcc; ARM upstream tarball). clang + lld also works for
# thumbv7m, but cross compiler-rt for armv7m isn't on every distro —
# sticking with the GCC toolchain keeps CI deps to one apt package.
set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR           arm-none-eabi-ar)
set(CMAKE_RANLIB       arm-none-eabi-ranlib)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_OBJDUMP      arm-none-eabi-objdump)
set(CMAKE_NM           arm-none-eabi-nm)

# CMake's default compiler-test step tries to LINK a tiny executable.
# In bare-metal mode that needs a linker script + startup file we
# don't ship. Static-library mode skips the link.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -mcpu=cortex-m3 selects the lowest-common-denominator thumbv7m core
# (no FPU, integer divide instruction present). The same .a works on
# M4/M4F/M7 — they're forward-compatible. -mfloat-abi=soft routes all
# float ops through libgcc soft-float helpers, matching the
# soft-float-via-libgcc story documented in NEXT.md.
set(_cm_flags "-mcpu=cortex-m3 -mthumb -mfloat-abi=soft -ffreestanding -fno-builtin")
set(CMAKE_C_FLAGS_INIT   "${_cm_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_cm_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_cm_flags}")

# Marker variable: top-level CMakeLists.txt checks this to skip
# host-only targets (test executables, cvm-cc, embedder/installed
# fixtures, install rules) and to wire in the nm allowlist check.
set(CVM_EMBEDDED_SANITY_BUILD ON CACHE BOOL
    "Building only the bare-metal cross-compile sanity target" FORCE)
