message(STATUS "Will build RNNoise")

# RNNoise builds via its own autotools ./configure && make invocation
# below, which is a separate build system CMake just shells out to -- it
# never sees CMAKE_C_COMPILER_LAUNCHER/CMAKE_CXX_COMPILER_LAUNCHER (e.g.
# ccache), since those only apply to CMake's own compile rules. Bake the
# launcher into CC/CXX instead so RNNoise's build gets cached the same way
# as the rest of the project.
if(CMAKE_C_COMPILER_LAUNCHER)
    set(RNNOISE_CC "${CMAKE_C_COMPILER_LAUNCHER} ${CMAKE_C_COMPILER}")
else()
    set(RNNOISE_CC "${CMAKE_C_COMPILER}")
endif()

if(CMAKE_CXX_COMPILER_LAUNCHER)
    set(RNNOISE_CXX "${CMAKE_CXX_COMPILER_LAUNCHER} ${CMAKE_CXX_COMPILER}")
else()
    set(RNNOISE_CXX "${CMAKE_CXX_COMPILER}")
endif()

set(CONFIGURE_COMMAND ./autogen.sh && ./configure --with-pic --disable-examples --disable-doc --disable-shared CC=${RNNOISE_CC} CXX=${RNNOISE_CXX})

if (CMAKE_CROSSCOMPILING)
set(CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --host=${CMAKE_C_COMPILER_TARGET} --target=${CMAKE_C_COMPILER_TARGET})
endif (CMAKE_CROSSCOMPILING)

set(RNNOISE_REPO https://github.com/xiph/rnnoise.git)

# Pinned to a specific commit (rather than the floating "main" branch) because
# a local patch is applied below (see
# cmake/patches/rnnoise-sparse-sgemv8x4-neon.patch). A patch generated against
# one commit is not guaranteed to apply cleanly against whatever "main" happens
# to point to later, so the two need to move together: bumping this SHA means
# regenerating/re-verifying the patch against the new commit.
set(RNNOISE_GIT_TAG 70f1d256acd4b34a572f999a05c87bf00b67730d)

# vec_neon.h's sparse_sgemv8x4() (used for RNNoise's pruned GRU/dense weight
# matrices -- the dominant cost in rnnoise_process_frame) is a plain scalar
# loop upstream, explicitly marked "Temporarily use unoptimized version";
# vec_avx.h's version of the same function is properly AVX2-vectorized. This
# patch ports that same 8-row/4-column-unrolled approach to NEON (as two
# float32x4_t accumulators, since NEON is 128-bit vs AVX's 256-bit), matching
# the already-vectorized (and unaffected) sgemv8x1() a few lines above it in
# the same file. Only affects the NEON branch of vec.h's arch dispatch, so it
# has no effect on x86 builds.
set(RNNOISE_PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH_FILE=${CMAKE_CURRENT_LIST_DIR}/patches/rnnoise-sparse-sgemv8x4-neon.patch -P ${CMAKE_CURRENT_LIST_DIR}/patches/apply_if_needed.cmake)

include(ExternalProject)

if(APPLE)
set(RNNOISE_APPLE_MIN_BUILD -mmacosx-version-min=11.0)
# autoconf's compiler-works check invokes CC (which CMake may resolve to the
# Xcode toolchain's raw absolute cc path rather than /usr/bin/cc or `xcrun
# cc`) without going through xcrun, so it doesn't auto-discover the default
# SDK and fails with "ld: library 'System' not found". Pass -isysroot
# explicitly so RNNoise's ./configure can actually link its test program.
if(CMAKE_OSX_SYSROOT)
    set(RNNOISE_APPLE_FLAGS -isysroot\ ${CMAKE_OSX_SYSROOT}\ ${RNNOISE_APPLE_MIN_BUILD})
else()
    set(RNNOISE_APPLE_FLAGS ${RNNOISE_APPLE_MIN_BUILD})
endif(CMAKE_OSX_SYSROOT)
endif(APPLE)

if(APPLE AND BUILD_OSX_UNIVERSAL)
# RNNoise ./configure doesn't behave properly when built as a universal binary;
# build it twice and use lipo to create a universal librnnoise.a instead.
ExternalProject_Add(build_rnnoise_x86
    DOWNLOAD_EXTRACT_TIMESTAMP NO
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --enable-x86-rtcd --host=x86_64-apple-darwin --target=x86_64-apple-darwin CFLAGS=-arch\ x86_64\ -O2\ ${RNNOISE_APPLE_FLAGS}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    GIT_REPOSITORY ${RNNOISE_REPO}
    GIT_TAG ${RNNOISE_GIT_TAG}
    UPDATE_DISCONNECTED 1
    PATCH_COMMAND ${RNNOISE_PATCH_COMMAND}
)
ExternalProject_Add(build_rnnoise_arm
    DOWNLOAD_EXTRACT_TIMESTAMP NO
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --host=aarch64-apple-darwin --target=aarch64-apple-darwin CFLAGS=-arch\ arm64\ -O2\ ${RNNOISE_APPLE_FLAGS}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    GIT_REPOSITORY ${RNNOISE_REPO}
    GIT_TAG ${RNNOISE_GIT_TAG}
    UPDATE_DISCONNECTED 1
    PATCH_COMMAND ${RNNOISE_PATCH_COMMAND}
)

ExternalProject_Get_Property(build_rnnoise_arm BINARY_DIR)
ExternalProject_Get_Property(build_rnnoise_arm SOURCE_DIR)
set(RNNOISE_ARM_BINARY_DIR ${BINARY_DIR})
ExternalProject_Get_Property(build_rnnoise_x86 BINARY_DIR)
set(RNNOISE_X86_BINARY_DIR ${BINARY_DIR})

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}
    COMMAND lipo ${RNNOISE_ARM_BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX} ${RNNOISE_X86_BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX} -output ${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX} -create
    DEPENDS build_rnnoise_arm build_rnnoise_x86)

add_custom_target(
    librnnoise.a
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX})

include_directories(${SOURCE_DIR}/include)

add_library(rnnoise STATIC IMPORTED)
add_dependencies(rnnoise librnnoise.a)
set_target_properties(rnnoise PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

add_library(rnnoise_inc INTERFACE)
target_include_directories(rnnoise_inc INTERFACE ${SOURCE_DIR}/include)

else(APPLE AND BUILD_OSX_UNIVERSAL)

if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86")
message(STATUS "RNNoise: Enabling optimizations if available on user's system")
set(CONFIGURE_COMMAND ${CONFIGURE_COMMAND} --enable-x86-rtcd)
endif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "x86")

if(APPLE)
set(CONFIGURE_COMMAND ${CONFIGURE_COMMAND} CFLAGS=-O2\ ${RNNOISE_APPLE_FLAGS})
endif(APPLE)

ExternalProject_Add(build_rnnoise
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ${CONFIGURE_COMMAND}
    BUILD_COMMAND $(MAKE)
    INSTALL_COMMAND ""
    GIT_REPOSITORY ${RNNOISE_REPO}
    GIT_TAG ${RNNOISE_GIT_TAG}
    UPDATE_DISCONNECTED 1
    PATCH_COMMAND ${RNNOISE_PATCH_COMMAND}
)

ExternalProject_Get_Property(build_rnnoise BINARY_DIR)
ExternalProject_Get_Property(build_rnnoise SOURCE_DIR)
add_library(rnnoise STATIC IMPORTED)
add_dependencies(rnnoise build_rnnoise)

add_library(rnnoise_inc INTERFACE)
target_include_directories(rnnoise_inc INTERFACE ${SOURCE_DIR}/include)

set_target_properties(rnnoise PROPERTIES
    IMPORTED_LOCATION "${BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}"
    IMPORTED_IMPLIB   "${BINARY_DIR}/.libs/librnnoise${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

include_directories(${SOURCE_DIR}/include)
endif(APPLE AND BUILD_OSX_UNIVERSAL)
