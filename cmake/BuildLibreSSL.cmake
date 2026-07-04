set(LIBRESSL_VERSION "4.3.2")

# LibreSSL configuration options
set(LIBRESSL_SKIP_INSTALL ON)
set(LIBRESSL_APPS OFF)
set(LIBRESSL_TESTS OFF)
set(ENABLE_ASM OFF)
set(OPENSSLDIR "/etc/ssl")

include(FetchContent)
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.28.0")
    FetchContent_Declare(
        LibreSSL
        URL https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz
        URL_HASH SHA256=edf01aee24c65d69e6a9efcb9d44bcda682ff9d4f3bbbd95e794e1dfa90847b5
        EXCLUDE_FROM_ALL
    )

    FetchContent_MakeAvailable(LibreSSL)
else()
    FetchContent_Declare(
        LibreSSL
        URL https://ftp.openbsd.org/pub/OpenBSD/LibreSSL/libressl-${LIBRESSL_VERSION}.tar.gz
        URL_HASH SHA256=edf01aee24c65d69e6a9efcb9d44bcda682ff9d4f3bbbd95e794e1dfa90847b5
    )
    FetchContent_GetProperties(LibreSSL)
    if(NOT LibreSSL_POPULATED)
        FetchContent_Populate(LibreSSL)
        add_subdirectory(${libressl_SOURCE_DIR} ${libressl_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
endif()

#target_include_directories(LibreSSL BEFORE PRIVATE ${LibreSSL_BINARY_DIR})
include_directories(${libressl_SOURCE_DIR}/include)
