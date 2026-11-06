# OpenCascade (OCCT) compiled into judas as static libraries, for Engineering mode (src/cad/cad_occt.cpp).
# Built from the pinned official release tag by an ExternalProject (own build tree, own install prefix), then linked
# into judas_core. No system OCCT, no Python, no runtime download: the download happens at build time only.
#
# Options:
#   JUDAS_OCCT_TAG         release tag of github.com/Open-Cascade-SAS/OCCT (default V7_9_3)
#   JUDAS_OCCT_SOURCE_DIR  use an existing checkout of that tag instead of cloning
#   JUDAS_OCCT_PREFIX      use an already installed static build (skips building OCCT), e.g. from another build dir
include(ExternalProject)

set(JUDAS_OCCT_TAG "V7_9_3" CACHE STRING "OCCT release tag")
set(JUDAS_OCCT_SOURCE_DIR "" CACHE PATH "Existing OCCT checkout (optional)")
set(JUDAS_OCCT_PREFIX "" CACHE PATH "Existing OCCT static install prefix (optional)")

# Modeling data/algorithms plus the STEP and STL readers/writers. TKService, TKV3d and the OCAF toolkits are
# here only because TKXCAF (needed by TKDESTEP and TKDESTL) links them; no OpenGL, FreeType or Tk is used.
set(JUDAS_OCCT_TOOLKITS
    TKDESTEP TKDESTL TKXCAF TKVCAF TKCAF TKLCAF TKCDF TKXSBase TKDE
    TKV3d TKService TKOffset TKFeat TKFillet TKHLR TKMesh TKXMesh TKBO TKBool TKShHealing TKPrim
    TKTopAlgo TKGeomAlgo TKBRep TKGeomBase TKG3d TKG2d TKMath TKernel)

if(JUDAS_OCCT_PREFIX)
    set(_occt_prefix ${JUDAS_OCCT_PREFIX})
else()
    set(_occt_prefix ${CMAKE_BINARY_DIR}/occt-install)
endif()
set(_occt_libs "")
foreach(t ${JUDAS_OCCT_TOOLKITS})
    list(APPEND _occt_libs ${_occt_prefix}/lib/lib${t}.a)
endforeach()

if(NOT JUDAS_OCCT_PREFIX)
    if(JUDAS_OCCT_SOURCE_DIR)
        set(_occt_src SOURCE_DIR ${JUDAS_OCCT_SOURCE_DIR})
    else()
        set(_occt_src GIT_REPOSITORY https://github.com/Open-Cascade-SAS/OCCT.git GIT_TAG ${JUDAS_OCCT_TAG}
                      GIT_SHALLOW TRUE SOURCE_DIR ${CMAKE_BINARY_DIR}/occt-src)
    endif()
    string(REPLACE ";" "$<SEMICOLON>" _occt_extra "TKService;TKV3d;TKCDF;TKLCAF;TKCAF;TKVCAF;TKXCAF;TKDE;TKXSBase;TKDESTEP;TKDESTL")
    ExternalProject_Add(occt_ext
        ${_occt_src}
        PREFIX ${CMAKE_BINARY_DIR}/occt-ep
        BINARY_DIR ${CMAKE_BINARY_DIR}/occt-build
        INSTALL_DIR ${_occt_prefix}
        CMAKE_GENERATOR ${CMAKE_GENERATOR}
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${_occt_prefix}
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DBUILD_LIBRARY_TYPE=Static
            -DBUILD_CPP_STANDARD=C++17
            -DBUILD_MODULE_FoundationClasses=ON
            -DBUILD_MODULE_ModelingData=ON
            -DBUILD_MODULE_ModelingAlgorithms=ON
            -DBUILD_MODULE_Visualization=OFF
            -DBUILD_MODULE_ApplicationFramework=OFF
            -DBUILD_MODULE_DataExchange=OFF
            -DBUILD_MODULE_DETools=OFF
            -DBUILD_MODULE_Draw=OFF
            -DBUILD_ADDITIONAL_TOOLKITS=${_occt_extra}
            -DBUILD_DOC_Overview=OFF
            -DUSE_FREETYPE=OFF -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_XLIB=OFF -DUSE_OPENGL=OFF
            -DUSE_GLES2=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_TBB=OFF -DUSE_FREEIMAGE=OFF
            -DUSE_FFMPEG=OFF -DUSE_OPENVR=OFF -DUSE_EIGEN=OFF
        BUILD_BYPRODUCTS ${_occt_libs}
        USES_TERMINAL_BUILD FALSE)
endif()

file(MAKE_DIRECTORY ${_occt_prefix}/include/opencascade)
add_library(judas_occt INTERFACE)
target_include_directories(judas_occt SYSTEM INTERFACE ${_occt_prefix}/include/opencascade)
target_link_libraries(judas_occt INTERFACE -Wl,--start-group ${_occt_libs} -Wl,--end-group Threads::Threads ${CMAKE_DL_LIBS})
if(NOT JUDAS_OCCT_PREFIX)
    add_dependencies(judas_occt occt_ext)
endif()
