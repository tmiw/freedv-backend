add_library(fdv_reporting STATIC
    pskreporter.cpp
    pskreporter.h
    FreeDVReporter.cpp
    FreeDVReporter.h
    CsvReporter.cpp
    CsvReporter.h
    UdpReporter.cpp
    UdpReporter.h
)

target_compile_options(fdv_reporting PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_reporting PRIVATE yyjson git_version)
target_include_directories(fdv_reporting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT DISABLE_TLS_SUPPORT)
    target_compile_definitions(fdv_reporting PRIVATE ENABLE_TLS_SUPPORT)
endif(NOT DISABLE_TLS_SUPPORT)

if(BUILD_BACKEND_UNITTESTS)
macro(DefineUnitTest utName)
    add_executable(${utName} test/${utName}.cpp)
    target_link_libraries(${utName} PRIVATE fdv_reporting fdv_util fdv_os_wrapper)
    target_include_directories(${utName} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

    add_test(NAME reporting_${utName} COMMAND ${CMAKE_CURRENT_BINARY_DIR}/${utName})
endmacro()

DefineUnitTest(CsvReporterTest)
DefineUnitTest(UdpReporterTest)
DefineUnitTest(PskReporterTest)
DefineUnitTest(FreeDVReporterTest)
endif(BUILD_BACKEND_UNITTESTS)
