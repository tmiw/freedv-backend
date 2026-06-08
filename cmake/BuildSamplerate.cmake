set(SAMPLERATE_VERSION "0.2.2")

set(BUILD_TESTING OFF CACHE BOOL "Enable unit tests for libsamplerate")

include(FetchContent)
FetchContent_Declare(
    samplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_TAG        master
)

FetchContent_MakeAvailable(samplerate)
target_compile_options(samplerate PRIVATE -g -O3) # Ensure that samplerate is built with optimizations

target_include_directories(samplerate BEFORE PRIVATE ${samplerate_BINARY_DIR})
include_directories(${samplerate_SOURCE_DIR}/include)
