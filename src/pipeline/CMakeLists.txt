add_library(fdv_audio_pipeline STATIC
    AgcStep.cpp
    AudioPipeline.cpp
    BandwidthExpandStep.cpp
    EitherOrStep.cpp
    IPipelineStep.cpp
    LevelAdjustStep.cpp
    LinkStep.cpp
    MinimalRealTimeHelper.cpp
    MinimalTxRxThread.cpp
    MuteStep.cpp
    RADEReceiveStep.cpp
    RADETransmitStep.cpp
    ResampleStep.cpp
    RNNoiseStep.cpp
    TapStep.cpp
    ToneInterfererStep.cpp
    rade_text.cpp
    ldpc_encode.cpp
    ldpc_decode.cpp
)

add_dependencies(fdv_audio_pipeline rade opus)
target_compile_options(fdv_audio_pipeline PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_audio_pipeline PRIVATE ${LIBEBUR128} agc rnnoise fdv_os_wrapper samplerate fdv_util)
target_link_libraries(fdv_audio_pipeline INTERFACE rade opus rnnoise)
target_include_directories(fdv_audio_pipeline PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)
target_include_directories(fdv_audio_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_BACKEND_UNITTESTS)
macro(DefineUnitTest utName)
    add_executable(${utName} test/${utName}.cpp)
    target_link_libraries(${utName} PRIVATE fdv_audio_pipeline fdv_util samplerate)
    target_include_directories(${utName} PRIVATE ${CODEC2_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)

    add_test(NAME pipeline_${utName} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${utName})

    set_target_properties(
        ${utName}
        PROPERTIES BUILD_RPATH "${CMAKE_BUILD_RPATH};${rade_BINARY_DIR}/src")
endmacro()

DefineUnitTest(AudioPipelineTest)
DefineUnitTest(EitherOrTest)
DefineUnitTest(LevelAdjustTest)
DefineUnitTest(ResampleTest)
DefineUnitTest(TapTest)

DefineUnitTest(ldpc_decode)
DefineUnitTest(ldpc_encode)

DefineUnitTest(RadeTextTest)
target_link_libraries(RadeTextTest PRIVATE rade opus samplerate)
#endif(LINUX)

# ---------------------------------------------------------------------------
# MinimalTxRxThreadTest – end-to-end TX→RX pipeline quality check via loss.py
#
# Requires Python3 with numpy and torch installed.  Set RADAE_PYTHON_EXECUTABLE
# on the cmake command line to override the auto-detected interpreter, e.g.:
#   cmake -DRADAE_PYTHON_EXECUTABLE=/path/to/venv/bin/python3 ..
# ---------------------------------------------------------------------------
set(RADAE_PYTHON_EXECUTABLE "" CACHE STRING
    "Python3 interpreter (with numpy/torch) for MinimalTxRxThreadTest. Auto-detected if empty.")

if(NOT RADAE_PYTHON_EXECUTABLE)
    # Try the system Python3 first
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c "import numpy, torch"
            RESULT_VARIABLE _PYTHON_DEPS_OK
            OUTPUT_QUIET ERROR_QUIET)
        if(_PYTHON_DEPS_OK EQUAL 0)
            set(RADAE_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
        endif()
    endif()
endif()

if(NOT RADAE_PYTHON_EXECUTABLE)
    # Fall back to common virtual-environment locations
    foreach(_PY_CANDIDATE
        "$ENV{HOME}/radae/radae-venv/bin/python3"
        "${CMAKE_BINARY_DIR}/radae-venv/bin/python3"
        "/opt/homebrew/bin/python3"
        "/usr/local/bin/python3")
        if(EXISTS "${_PY_CANDIDATE}")
            execute_process(
                COMMAND "${_PY_CANDIDATE}" -c "import numpy, torch"
                RESULT_VARIABLE _PY_OK
                OUTPUT_QUIET ERROR_QUIET)
            if(_PY_OK EQUAL 0)
                set(RADAE_PYTHON_EXECUTABLE "${_PY_CANDIDATE}")
                break()
            endif()
        endif()
    endforeach()
endif()

if(RADAE_PYTHON_EXECUTABLE)
    message(STATUS "MinimalTxRxThreadTest: using Python at ${RADAE_PYTHON_EXECUTABLE}")

    DefineUnitTest(MinimalTxRxThreadTest)
    target_link_libraries(MinimalTxRxThreadTest PRIVATE rade opus samplerate)
    target_compile_definitions(MinimalTxRxThreadTest PRIVATE
        RADE_SRC_DIR="${rade_SOURCE_DIR}"
        PYTHON_EXECUTABLE="${RADAE_PYTHON_EXECUTABLE}")
    # Allow up to 1 hour: 6 rate combinations × ~10 min each (RADE NN inference
    # on CPU can be significantly slower than real-time).
    set_tests_properties(pipeline_MinimalTxRxThreadTest PROPERTIES TIMEOUT 3600)
else()
    message(STATUS "MinimalTxRxThreadTest: no suitable Python3 found – test will be skipped")
endif()

endif(BUILD_BACKEND_UNITTESTS)
