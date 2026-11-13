# Toolchain for the standalone Windows x64 build (GitHub Actions runner).
#
# MSVC is used instead of clang because:
#   * DualMoE.exe is a GUI-subsystem native window app built on the Edge
#     WebView2 COM headers, which are only provided for clang-cl / MSVC
#     (the clang driver cannot parse the MIDL-generated WebView2.h);
#   * the whole binary is linked with the static MSVC runtime (/MT) so the
#     release is a single self-contained .exe.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   "${CMAKE_C_COMPILER}"   CACHE STRING "")
set(CMAKE_CXX_COMPILER "${CMAKE_CXX_COMPILER}" CACHE STRING "")

# MSVC must be the generator's compiler (Ninja + cl.exe); clang-cl is accepted
# as a drop-in replacement because it shares the same command line and headers.
if (NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
    find_program(DUALMOE_CL NAMES cl clang-cl)
    if (DUALMOE_CL)
        set(CMAKE_C_COMPILER   "${DUALMOE_CL}")
        set(CMAKE_CXX_COMPILER "${DUALMOE_CL}")
    endif()
endif()

# Static CRT by default: no vcruntime140.dll / msvcp140.dll next to the .exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "")

# Portable CPU baseline (Ryzen 7 5700X): AVX2 + FMA + F16C without -march=native.
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_AVX2   ON  CACHE BOOL "" FORCE)
set(GGML_FMA    ON  CACHE BOOL "" FORCE)
set(GGML_F16C   ON  CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
