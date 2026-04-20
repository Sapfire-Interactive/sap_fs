cmake_minimum_required(VERSION 3.20)
project(sap_fs
    VERSION 0.1.0
    DESCRIPTION "Filesystem library for Sap projects "
    LANGUAGES C CXX
)

# Guard against being added more than once via nested submodules.
if(TARGET sap::fs)
    return()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_SHARED_LIBS "Should build shared library instead" Off)
option(BUILD_TESTS "Should build tests" On)
option(SAP_FS_INSTALL "Should install" Off)

set(SAP_FS_SOURCES
    src/fs.cpp
    $<$<PLATFORM_ID:Windows>:src/file_windows.cpp>
    $<$<PLATFORM_ID:Linux>:src/file_posix.cpp>
    $<$<PLATFORM_ID:Windows>:src/mapped_file_windows.cpp>
    $<$<PLATFORM_ID:Linux>:src/mapped_file_posix.cpp>
)

if(BUILD_SHARED_LIBS)
add_library(sap_fs SHARED
    ${SAP_FS_SOURCES}
)
else()
add_library(sap_fs STATIC
    ${SAP_FS_SOURCES}
)
endif()

add_library(sap::fs ALIAS sap_fs)

target_include_directories(sap_fs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

add_subdirectory(sap_core)

target_link_libraries(sap_fs
    PUBLIC
        sap::core
)

target_compile_features(sap_fs PUBLIC cxx_std_20)

# Compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sap_fs PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(sap_fs PRIVATE /W4)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SAP_FS_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS sap_fs
        EXPORT sap_fs-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(EXPORT sap_fs-targets
        FILE sap_fs-targets.cmake
        NAMESPACE sap::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sap_fs
    )
endif()