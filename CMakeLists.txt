cmake_minimum_required(VERSION 3.20)
project(judas CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Phase 0/1 link the local llama.cpp build. Packaging later vendors llama.cpp as a submodule.
set(LLAMA_DIR "$ENV{HOME}/src/llama.cpp" CACHE PATH "llama.cpp source tree")
set(LLAMA_LIB_DIR "${LLAMA_DIR}/build/bin" CACHE PATH "directory with libllama.so")

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# llama.cpp: libllama + libllama-common (chat templates, tool-call parsing, sampling) and its vendored headers
add_library(llama_deps INTERFACE)
target_include_directories(llama_deps INTERFACE
    ${LLAMA_DIR}/include ${LLAMA_DIR}/ggml/include ${LLAMA_DIR}/common ${LLAMA_DIR}/vendor ${LLAMA_DIR}/tools/mtmd)
target_link_directories(llama_deps INTERFACE ${LLAMA_LIB_DIR})
target_link_libraries(llama_deps INTERFACE llama-common mtmd llama ggml ggml-base Threads::Threads)

# cpp-httplib: libllama-common already exports it (built with OpenSSL). Use that copy with the SAME defines;
# a second copy with other defines breaks the class layouts at runtime (Release crash, 2026-10-13).
find_package(OpenSSL REQUIRED)
target_compile_definitions(llama_deps INTERFACE
    CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=1048576 CPPHTTPLIB_LISTEN_BACKLOG=512
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=32768 CPPHTTPLIB_TCP_NODELAY=1)
target_link_libraries(llama_deps INTERFACE OpenSSL::SSL OpenSSL::Crypto)

# sha256 from llama.cpp's vendored hash code
add_library(judas_sha256 STATIC ${LLAMA_DIR}/vendor/hash/sha256/sha256.c)
target_include_directories(judas_sha256 PUBLIC ${LLAMA_DIR}/vendor/hash)
set_target_properties(judas_sha256 PROPERTIES LINKER_LANGUAGE C)

# Embed web/ into the binary: regenerate assets_generated.cpp whenever any web file changes.
file(GLOB_RECURSE WEB_FILES CONFIGURE_DEPENDS web/*)
set(ASSETS_CPP ${CMAKE_BINARY_DIR}/assets_generated.cpp)
add_custom_command(
    OUTPUT ${ASSETS_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN_DIR=${CMAKE_SOURCE_DIR}/web -DOUT=${ASSETS_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    DEPENDS ${WEB_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_assets.cmake
    COMMENT "Embedding web/ assets")

# Embed skins/ (built-in themes) the same way.
file(GLOB SKIN_FILES CONFIGURE_DEPENDS skins/*.toml)
set(SKINS_CPP ${CMAKE_BINARY_DIR}/skins_generated.cpp)
add_custom_command(
    OUTPUT ${SKINS_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN_DIR=${CMAKE_SOURCE_DIR}/skins -DOUT=${SKINS_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_skins.cmake
    DEPENDS ${SKIN_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_skins.cmake
    COMMENT "Embedding skins/ themes")

# One static library for all of src/ except main.cpp (the TUI is raw ANSI + termios, no extra dependency).
file(GLOB_RECURSE JUDAS_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM JUDAS_SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)
# The built-in day planner (src/mach12) is self-contained and can be compiled out: -DJUDAS_MACH12=OFF.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/mach12/mach12.cpp)
    set(_mach12_default ON)
else()
    set(_mach12_default OFF)   # the module is not in this source tree
endif()
option(JUDAS_MACH12 "Build the built-in mach12 day planner server" ${_mach12_default})
if(NOT JUDAS_MACH12)
    list(FILTER JUDAS_SOURCES EXCLUDE REGEX "/src/mach12/")
endif()
# Strata backend (docs/STRATA-BACKEND.md): a second engine that drives `strata --serve` as a child process.
# OFF by default; the default build is unchanged. ON needs no ROCm at build time (the engine binary is Strata's own).
option(JUDAS_STRATA "Build the Strata inference backend into judas (needs ROCm and ~/src/strata)" OFF)
if(NOT JUDAS_STRATA)
    list(REMOVE_ITEM JUDAS_SOURCES ${CMAKE_SOURCE_DIR}/src/engine/strata_engine.cpp)
endif()
add_library(judas_core STATIC ${JUDAS_SOURCES} ${ASSETS_CPP} ${SKINS_CPP})
target_include_directories(judas_core PUBLIC src)
if(JUDAS_MACH12)
    target_include_directories(judas_core PUBLIC src/mach12)   # the TUI picks up tui_planner.h through JUDAS_TUI_PLANNER
endif()
target_link_libraries(judas_core PUBLIC llama_deps judas_sha256 SQLite3::SQLite3)
if(JUDAS_MACH12)
    target_compile_definitions(judas_core PUBLIC JUDAS_MACH12=1 JUDAS_TUI_PLANNER=1)
endif()
if(JUDAS_STRATA)
    target_compile_definitions(judas_core PUBLIC JUDAS_WITH_STRATA=1)
endif()
target_compile_options(judas_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(judas src/main.cpp)
target_link_libraries(judas PRIVATE judas_core)
set_target_properties(judas PROPERTIES BUILD_RPATH ${LLAMA_LIB_DIR} INSTALL_RPATH ${LLAMA_LIB_DIR})

# Tests: every tests/test_*.cpp is its own executable; `ctest` runs them all.
enable_testing()
set(JUDAS_TEST_TMP "/tmp/judas-tests" CACHE STRING "Scratch directory the tests write under")
file(GLOB JUDAS_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
if(NOT JUDAS_MACH12)
    list(FILTER JUDAS_TESTS EXCLUDE REGEX "test_mach12.*\\.cpp$")
endif()
foreach(t ${JUDAS_TESTS})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t})
    target_link_libraries(${name} PRIVATE judas_core)
    set_target_properties(${name} PROPERTIES BUILD_RPATH ${LLAMA_LIB_DIR})
    target_compile_definitions(${name} PRIVATE JUDAS_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests" JUDAS_TEST_TMP="${JUDAS_TEST_TMP}")
    add_test(NAME ${name} COMMAND ${name})
endforeach()

# Every shipped theme must pass the WCAG contrast thresholds (tools/skin-contrast.py).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME skin_contrast COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/skin-contrast.py ${CMAKE_SOURCE_DIR}/skins)
endif()

# Strata tokenizer check: CPU only (vocab-only model), skips itself when the Strata GGUF is absent.
if(JUDAS_STRATA)
    add_executable(strata_tokenizer_check tests/strata_tokenizer_check.cpp)
    target_link_libraries(strata_tokenizer_check PRIVATE judas_core)
    set_target_properties(strata_tokenizer_check PROPERTIES BUILD_RPATH ${LLAMA_LIB_DIR})
    target_compile_definitions(strata_tokenizer_check PRIVATE JUDAS_TESTS_DIR="${CMAKE_SOURCE_DIR}/tests" JUDAS_TEST_TMP="${JUDAS_TEST_TMP}")
    add_test(NAME strata_tokenizer_check COMMAND strata_tokenizer_check)
endif()

# Live tests need the GPU and a real model: build them, run them by hand under the bench lock (they load the model, so stop any running daemon first).
file(GLOB JUDAS_LIVE CONFIGURE_DEPENDS tests/live_*.cpp)
foreach(t ${JUDAS_LIVE})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t})
    target_link_libraries(${name} PRIVATE judas_core)
    set_target_properties(${name} PROPERTIES BUILD_RPATH ${LLAMA_LIB_DIR})
    target_compile_definitions(${name} PRIVATE JUDAS_TEST_TMP="${JUDAS_TEST_TMP}")
endforeach()

# Phase 0 engine spike (development checkouts only)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/spike/engine_spike.cpp)
    add_executable(engine_spike spike/engine_spike.cpp)
    target_link_libraries(engine_spike PRIVATE llama_deps)
    set_target_properties(engine_spike PROPERTIES BUILD_RPATH ${LLAMA_LIB_DIR})
endif()

# Strata engine compiled into judas (needs ROCm); must stay last: it silences add_test for Strata's own targets.
if(JUDAS_STRATA)
    include(cmake/strata.cmake)
endif()
