if(APPLE)
    set(OS_WRAP_FILES osx_interface.mm)
elseif(WIN32)
    set(OS_WRAP_FILES windows_interface.cpp)
else()
    set(OS_WRAP_FILES osx_stubs.cpp)
endif()

add_library(fdv_os_wrapper STATIC
    ${OS_WRAP_FILES}
)

if(APPLE)
target_link_libraries(fdv_os_wrapper "-framework AppKit -framework CoreGraphics -framework CoreFoundation -framework AVFoundation")
endif(APPLE)

target_compile_options(fdv_os_wrapper PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_include_directories(fdv_os_wrapper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/..)

if(BUILD_BACKEND_UNITTESTS)
    add_executable(OsInterfaceTest test/OsInterfaceTest.cpp)
    target_link_libraries(OsInterfaceTest PRIVATE fdv_os_wrapper)
    if(APPLE)
        target_link_libraries(OsInterfaceTest PRIVATE "-framework IOKit")
    else(APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(OsInterfaceTest PRIVATE Threads::Threads)
    endif(APPLE)
    add_test(NAME os_OsInterfaceTest COMMAND ${CMAKE_CURRENT_BINARY_DIR}/OsInterfaceTest)
    set_tests_properties(os_OsInterfaceTest PROPERTIES TIMEOUT 600)
endif(BUILD_BACKEND_UNITTESTS)
