cmake_minimum_required(VERSION 3.20)
project(cronovm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(cvm STATIC src/cvm.c)
target_include_directories(cvm PUBLIC include)
target_compile_options(cvm PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

enable_testing()
add_executable(test_vm tests/test_vm.c)
target_link_libraries(test_vm PRIVATE cvm)
target_compile_options(test_vm PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)
add_test(NAME vm_basic COMMAND test_vm)

add_executable(test_e2e tests/test_e2e.c)
target_link_libraries(test_e2e PRIVATE cvm)
target_compile_options(test_e2e PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

add_executable(test_regions tests/test_regions.c)
target_link_libraries(test_regions PRIVATE cvm)
target_compile_options(test_regions PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
    $<$<C_COMPILER_ID:MSVC>:/W4>
)

# --- Optional: translator (requires LLVM dev install with llvm-config) ----
add_subdirectory(tools/translator)

# --- cvm-cc: thin driver, no LLVM dependency. Always built. -----------
add_subdirectory(tools/cvm-cc)
