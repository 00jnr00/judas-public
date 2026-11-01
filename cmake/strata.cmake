# JUDAS_STRATA=ON: compile the Strata engine into judas itself. Strata's source tree (read only, never edited) is
# added as a subdirectory with its HIP backend; its src/program/generate.cpp, whose main() is the engine, is compiled
# one extra time here with `main` renamed to strata_main. `judas __strata-engine <strata args>` calls it, and
# StrataEngine starts /proc/self/exe with that subcommand, so the engine runs in its own process (VRAM is freed at
# exit) with no separate binary to ship. Needs ROCm (hipcc, hipblas) at build time; /opt/rocm is the default.
set(JUDAS_STRATA_DIR "$ENV{HOME}/src/strata" CACHE PATH "Strata source tree (MIT, github.com/Niko1221/Strata), used read only")
set(JUDAS_STRATA_ROCM "/opt/rocm" CACHE PATH "ROCm install prefix")
set(JUDAS_STRATA_HIP_ARCH "" CACHE STRING "GPU architecture for Strata's kernels (default: first gfx from rocminfo, else gfx1030)")

if(NOT EXISTS "${JUDAS_STRATA_DIR}/src/program/generate.cpp")
    message(FATAL_ERROR "JUDAS_STRATA=ON: no Strata source at ${JUDAS_STRATA_DIR} (set JUDAS_STRATA_DIR)")
endif()
if(NOT EXISTS "${JUDAS_STRATA_ROCM}/llvm/bin/clang++")
    message(FATAL_ERROR "JUDAS_STRATA=ON needs ROCm: ${JUDAS_STRATA_ROCM}/llvm/bin/clang++ not found (set JUDAS_STRATA_ROCM)")
endif()
if(NOT JUDAS_STRATA_HIP_ARCH)
    execute_process(COMMAND ${JUDAS_STRATA_ROCM}/bin/rocminfo OUTPUT_VARIABLE _rocminfo ERROR_QUIET)
    string(REGEX MATCH "gfx[0-9a-f]+" _arch "${_rocminfo}")
    if(NOT _arch)
        set(_arch gfx1030)
    endif()
    set(JUDAS_STRATA_HIP_ARCH "${_arch}" CACHE STRING "" FORCE)
endif()

# Strata's own switches, set before its CMakeLists runs.
set(STRATA_ENABLE_HIP ON)
set(STRATA_ENABLE_CUDA OFF)
set(STRATA_BUILD_TESTS OFF)
set(STRATA_PREFILL_MMQ OFF)
set(STRATA_GGML_DIR "${JUDAS_STRATA_DIR}/third_party/llama.cpp")   # Strata's pinned ggml, no network fetch
set(CMAKE_HIP_ARCHITECTURES "${JUDAS_STRATA_HIP_ARCH}" CACHE STRING "" FORCE)
set(CMAKE_HIP_COMPILER "${JUDAS_STRATA_ROCM}/llvm/bin/clang++" CACHE FILEPATH "" FORCE)
set(CMAKE_HIP_FLAGS "--rocm-path=${JUDAS_STRATA_ROCM} --rocm-device-lib-path=${JUDAS_STRATA_ROCM}/amdgcn/bitcode" CACHE STRING "" FORCE)
list(APPEND CMAKE_PREFIX_PATH "${JUDAS_STRATA_ROCM}")

# Strata registers its own ctest entries (hip_*); keep them out of judas's ctest.
function(add_test)
endfunction()
function(set_tests_properties)
endfunction()
add_subdirectory("${JUDAS_STRATA_DIR}" strata EXCLUDE_FROM_ALL)

get_target_property(_s_inc strata INCLUDE_DIRECTORIES)
get_directory_property(_s_defs DIRECTORY "${JUDAS_STRATA_DIR}" COMPILE_DEFINITIONS)
add_library(judas_strata_main STATIC "${JUDAS_STRATA_DIR}/src/program/generate.cpp")
set_target_properties(judas_strata_main PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(_s_inc)
    target_include_directories(judas_strata_main PRIVATE ${_s_inc})
endif()
target_include_directories(judas_strata_main PRIVATE "${JUDAS_STRATA_DIR}/include")
target_compile_definitions(judas_strata_main PRIVATE main=strata_main ${_s_defs})
target_link_libraries(judas_strata_main PUBLIC strata_engine strata_prefill strata_spec strata_hip_runtime)
# the ROCm runtime is found through the executable's rpath, like Strata's own binary
set_target_properties(judas PROPERTIES BUILD_RPATH "${LLAMA_LIB_DIR};${JUDAS_STRATA_ROCM}/lib" INSTALL_RPATH "${LLAMA_LIB_DIR};${JUDAS_STRATA_ROCM}/lib")

# The expert profile (a 190 KB data file) is embedded; StrataEngine writes it to <data_dir>/strata/ when
# models.<name>.strata_profile is not set.
file(READ "${JUDAS_STRATA_DIR}/data/expert-profile.bin" _prof HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _prof "${_prof}")
file(WRITE "${CMAKE_BINARY_DIR}/strata_profile_generated.cpp"
    "#include <cstddef>\nnamespace judas { extern const unsigned char strata_profile_data[]; extern const size_t strata_profile_size;\n"
    "const unsigned char strata_profile_data[] = {${_prof}0};\nconst size_t strata_profile_size = sizeof(strata_profile_data) - 1; }\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${JUDAS_STRATA_DIR}/data/expert-profile.bin")
target_sources(judas_core PRIVATE "${CMAKE_BINARY_DIR}/strata_profile_generated.cpp")

target_link_libraries(judas_core PUBLIC judas_strata_main)
# Strata links its own static ggml (same 0.24 line as llama.cpp's shared libggml). Keep the symbols of every static
# archive out of the executable's dynamic table so libllama.so keeps binding to its own libggml.
target_link_options(judas_core PUBLIC -Wl,--exclude-libs,ALL)
