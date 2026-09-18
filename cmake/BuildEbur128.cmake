set(EBUR128_VERSION "1.2.6")

# Static libraries on some platforms need PIC
set(EBUR128_CMAKE_ARGS -DWITH_STATIC_PIC=1 -DBUILD_SHARED_LIBS=0)

if(APPLE)
    set(EBUR128_CMAKE_ARGS ${EBUR128_CMAKE_ARGS} -DCMAKE_AR=${CMAKE_AR} -DCMAKE_RANLIB=${CMAKE_RANLIB})
endif(APPLE)

if(CMAKE_CROSSCOMPILING)
    set(EBUR128_CMAKE_ARGS ${EBUR128_CMAKE_ARGS} -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
endif()

# libebur128 is itself a CMake project, built here as a nested
# ExternalProject configure/build rather than via add_subdirectory(). That
# nested cmake invocation gets its own fresh CMakeCache.txt and doesn't
# inherit the parent project's CMAKE_C_COMPILER_LAUNCHER/
# CMAKE_CXX_COMPILER_LAUNCHER (e.g. ccache) automatically, so forward it
# explicitly.
if(CMAKE_C_COMPILER_LAUNCHER)
    set(EBUR128_CMAKE_ARGS ${EBUR128_CMAKE_ARGS} -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER})
endif()
if(CMAKE_CXX_COMPILER_LAUNCHER)
    set(EBUR128_CMAKE_ARGS ${EBUR128_CMAKE_ARGS} -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER})
endif()

# Build ebur128 library
include(ExternalProject)
ExternalProject_Add(build_ebur128
   SOURCE_DIR ebur128_src
   BINARY_DIR ebur128_build
   GIT_REPOSITORY https://github.com/jiixyj/libebur128.git
   GIT_TAG "v${EBUR128_VERSION}"
   GIT_SUBMODULES ""
   GIT_SUBMODULES_RECURSE NO
   CMAKE_ARGS ${EBUR128_CMAKE_ARGS}
   CMAKE_CACHE_ARGS -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET} -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}
   PATCH_COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Ebur128_CMake.patch
   INSTALL_COMMAND ""
   UPDATE_DISCONNECTED 1
)

ExternalProject_Get_Property(build_ebur128 BINARY_DIR)
ExternalProject_Get_Property(build_ebur128 SOURCE_DIR)
add_library(ebur128 STATIC IMPORTED)
add_dependencies(ebur128 build_ebur128)

set_target_properties(ebur128 PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/libebur128${CMAKE_STATIC_LIBRARY_SUFFIX}"
    IMPORTED_IMPLIB   "${BINARY_DIR}/libebur128${CMAKE_IMPORT_LIBRARY_SUFFIX}"
)

# src/pipeline/CMakeLists.txt links fdv_audio_pipeline against ${LIBEBUR128}.
# It must resolve to the "ebur128" imported target above (not a raw path) so
# that CMake's dependency graph actually orders fdv_audio_pipeline's build
# (headers included) after build_ebur128 -- otherwise nothing ties the two
# together and a parallel build can compile files that #include ebur128.h
# before it's even been cloned.
set(LIBEBUR128 ebur128)

set(EBUR128_INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR}/ebur128_src/ebur128 ${CMAKE_CURRENT_BINARY_DIR}/ebur128_build)
include_directories(${EBUR128_INCLUDE_DIRS})
