set(SAMPLERATE_VERSION "0.2.2")

set(BUILD_TESTING OFF CACHE BOOL "Enable unit tests for libsamplerate")

include(FetchContent)

# Pinned to a specific commit (rather than the floating "master" branch) because
# a local patch is applied below (see cmake/patches/libsamplerate-sinc-mono-perf.patch).
# A patch generated against one commit is not guaranteed to apply cleanly against
# whatever "master" happens to point to later, so the two need to move together:
# bumping this SHA means regenerating/re-verifying the patch against the new commit.
FetchContent_Declare(
    samplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_TAG        0844c208f683527c08ea8a80acc13b398aa9c8bf
    PATCH_COMMAND  ${CMAKE_COMMAND} -DPATCH_FILE=${CMAKE_CURRENT_LIST_DIR}/patches/libsamplerate-sinc-mono-perf.patch -P ${CMAKE_CURRENT_LIST_DIR}/patches/apply_if_needed.cmake
)

FetchContent_MakeAvailable(samplerate)
target_compile_options(samplerate PRIVATE -g -O3) # Ensure that samplerate is built with optimizations

target_include_directories(samplerate BEFORE PRIVATE ${samplerate_BINARY_DIR})
include_directories(${samplerate_SOURCE_DIR}/include)
