cmake_minimum_required(VERSION 3.20)
project(DualMoE VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/utf-8)
    # MSVC keeps __cplusplus at 199711L unless this flag is present; llama.cpp
    # relies on it to pick its UTF-8 literal handling (otherwise u8"" becomes
    # char8_t under /std:c++20 and fails to stream into std::stringstream).
    add_compile_options(/Zc:__cplusplus)
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(DUALMOE_BUILD_TESTS "Build DualMoE byte-identity test suite" ON)
set(LLAMA_SOURCE_DIR "" CACHE PATH "Path to patched llama.cpp source tree")
set(LLAMA_BUILD_DIR  "" CACHE PATH "Optional path to prebuilt llama.cpp build tree")

if (NOT LLAMA_SOURCE_DIR)
    include(FetchContent)
    set(DUALMOE_LLAMA_COMMIT "31953bcf74ba489a8bbb0c936c5fcf24fcf9485c")
    FetchContent_Declare(
        llama_cpp
        GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
        GIT_TAG        ${DUALMOE_LLAMA_COMMIT}
        PATCH_COMMAND  git apply --ignore-whitespace ${CMAKE_CURRENT_SOURCE_DIR}/patches/0001-dualmoe-parallel-seam.patch
    )
    set(LLAMA_BUILD_COMMON   ON  CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(llama_cpp)
    set(LLAMA_SOURCE_DIR "${llama_cpp_SOURCE_DIR}")
    set(DUALMOE_LLAMA_LIBS llama-common llama ggml cpp-httplib)
elseif (LLAMA_BUILD_DIR AND EXISTS "${LLAMA_BUILD_DIR}/src/libllama.a")
    add_library(dualmoe_llama_imported INTERFACE)
    target_link_libraries(dualmoe_llama_imported INTERFACE
        "${LLAMA_BUILD_DIR}/common/libllama-common.a"
        "${LLAMA_BUILD_DIR}/common/libllama-common-base.a"
        "${LLAMA_BUILD_DIR}/src/libllama.a"
        "${LLAMA_BUILD_DIR}/ggml/src/libggml.a"
        "${LLAMA_BUILD_DIR}/ggml/src/libggml-cpu.a"
        "${LLAMA_BUILD_DIR}/ggml/src/libggml-base.a"
        "${LLAMA_BUILD_DIR}/vendor/cpp-httplib/libcpp-httplib.a"
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    set(DUALMOE_LLAMA_LIBS dualmoe_llama_imported)
else()
    set(LLAMA_BUILD_COMMON   ON  CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LLAMA_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/llama-subbuild")
    if (NOT TARGET cpp-httplib)
        add_subdirectory("${LLAMA_SOURCE_DIR}/vendor/cpp-httplib" "${CMAKE_CURRENT_BINARY_DIR}/httplib-subbuild")
    endif()
    set(DUALMOE_LLAMA_LIBS llama-common llama ggml cpp-httplib)
    if (TARGET ggml-cpu)
        list(APPEND DUALMOE_LLAMA_LIBS ggml-cpu)
    endif()
    if (TARGET ggml-base)
        list(APPEND DUALMOE_LLAMA_LIBS ggml-base)
    endif()
    if (TARGET ggml-vulkan)
        list(APPEND DUALMOE_LLAMA_LIBS ggml-vulkan)
    endif()
endif()

# Embed DualMoE Studio HTML into a single C++ header
set(EMBEDDED_WEBUI_H "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_webui.h")
add_custom_command(
    OUTPUT  "${EMBEDDED_WEBUI_H}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_webui.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html"
            "${EMBEDDED_WEBUI_H}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/webui/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_webui.py"
    COMMENT "Embedding DualMoE Studio Web UI"
)

add_library(dualmoe_core STATIC
    src/gguf_inspect.cpp
    src/virtual_arena.cpp
    src/engine.cpp
    src/bench.cpp
)
target_include_directories(dualmoe_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${LLAMA_SOURCE_DIR}/include"
    "${LLAMA_SOURCE_DIR}/common"
    "${LLAMA_SOURCE_DIR}/vendor"
    "${LLAMA_SOURCE_DIR}/ggml/include"
)
target_compile_definitions(dualmoe_core PUBLIC
    _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_link_libraries(dualmoe_core PUBLIC ${DUALMOE_LLAMA_LIBS} Threads::Threads)
if (WIN32)
    target_link_libraries(dualmoe_core PUBLIC ws2_32 shell32)
endif()

add_library(dualmoe_app STATIC
    src/app_http.cpp
    src/win_dialogs.cpp
    "${EMBEDDED_WEBUI_H}"
)
target_include_directories(dualmoe_app PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${LLAMA_SOURCE_DIR}/vendor"
    "${LLAMA_SOURCE_DIR}/vendor/cpp-httplib"
)
target_link_libraries(dualmoe_app PUBLIC dualmoe_core)

# ---------------------------------------------------------------------------
# Native desktop window (Edge WebView2) — no browser required.
# Automatically enabled on Windows x64 when the webview dependency can be
# fetched; set -DDUALMOE_DESKTOP_WINDOW=OFF to build only the console server.
# ---------------------------------------------------------------------------
option(DUALMOE_DESKTOP_WINDOW "Build the native window app (DualMoE.exe, WebView2)" ON)
set(DUALMOE_WEBVIEW_TAG "0.12.0" CACHE STRING "webview/webview release tag")

if (DUALMOE_DESKTOP_WINDOW AND WIN32)
    include(FetchContent)
    # WEBVIEW_BUILD must stay ON: it is what triggers webview's dependency
    # discovery (MSWebView2 headers) and the core library target.
    set(WEBVIEW_BUILD               ON  CACHE BOOL "" FORCE)
    set(WEBVIEW_USE_COMPAT_MINGW    OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_STATIC_LIBRARY ON  CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_BUILD_DOCS          OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_INSTALL_TARGETS     OFF CACHE BOOL "" FORCE)
    set(WEBVIEW_USE_BUILTIN_MSWEBVIEW2 ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        webview_lib
        GIT_REPOSITORY https://github.com/webview/webview.git
        GIT_TAG        ${DUALMOE_WEBVIEW_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(webview_lib)

    if (DEFINED microsoft_web_webview2_SOURCE_DIR)
        set(MSWebView2_ROOT "${microsoft_web_webview2_SOURCE_DIR}")
    endif()

    # Icon + version metadata (title bar / taskbar / Explorer properties).
    set(DUALMOE_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/dualmoe.rc")
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/dualmoe.ico")
        find_package(Python3 COMPONENTS Interpreter)
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_icon.py"
                        RESULT_VARIABLE _icon_rc OUTPUT_QUIET ERROR_QUIET)
    endif()

    add_executable(DualMoE WIN32
        src/main_desktop.cpp
        ${DUALMOE_RC}
    )
    set_source_files_properties(${DUALMOE_RC} PROPERTIES
        COMPILE_DEFINITIONS ""
    )
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/dualmoe.ico")
        message(WARNING "DualMoE: assets/dualmoe.ico missing — EXE will use the default icon")
    endif()
    target_compile_definitions(DualMoE PRIVATE DUALMOE_DESKTOP=1 UNICODE _UNICODE)
    # webview::core_static provides webview/webview.h + the WebView2 COM headers
    # and links advapi32/ole32/shell32/shlwapi/user32/version.
    target_link_libraries(DualMoE PRIVATE dualmoe_app webview::core_static)
    set_target_properties(DualMoE PROPERTIES
        OUTPUT_NAME "DualMoE"
        WIN32_EXECUTABLE ON
    )
    # Standard Win32 GUI subsystem; the CRT picks wWinMainCRTStartup automatically
    # for a UNICODE windowed app, so no custom /ENTRY is needed.
    set_target_properties(DualMoE PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
    # Self-contained single .exe: static CRT + static WebView2 headers.
    if (MSVC)
        set_property(TARGET DualMoE PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_link_options(DualMoE PRIVATE /MANIFEST:EMBED)
    endif()
    message(STATUS "DualMoE: native window app enabled (webview ${DUALMOE_WEBVIEW_TAG}, WebView2 headers)")
else()
    message(STATUS "DualMoE: native window app disabled — building console server only")
endif()

# Headless console build (same UI in a browser) — always available, also on Linux.
add_executable(dualmoe-server
    src/main_desktop.cpp
)
target_link_libraries(dualmoe-server PRIVATE dualmoe_app)
if (WIN32)
    set_target_properties(dualmoe-server PROPERTIES OUTPUT_NAME "DualMoE-server")
endif()

add_executable(dualmoe-cli
    cli/main.cpp
)
target_link_libraries(dualmoe-cli PRIVATE dualmoe_core)

if (DUALMOE_BUILD_TESTS)
    enable_testing()
    add_executable(dualmoe-tests
        tests/test_parallel_moe.cpp
    )
    target_link_libraries(dualmoe-tests PRIVATE dualmoe_core)
endif()
