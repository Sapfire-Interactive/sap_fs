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

option(SAP_FS_SHARED "Should build shared library instead" OFF)
option(SAP_FS_INSTALL "Should install" Off)

if(SAP_FS_SHARED)
add_library(sap_fs SHARED
    src/fs.cpp
)
else()
add_library(sap_fs STATIC
    src/fs.cpp
)
endif()

add_library(sap::fs ALIAS sap_fs)

target_include_directories(sap_fs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

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