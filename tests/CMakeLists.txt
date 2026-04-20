cmake_minimum_required(VERSION 3.20)

# GTest is already fetched by sap_core (a subdirectory of the parent).
# By the time this file runs, GTest::gtest_main is available.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(sap_fs_tests
    file_tests.cpp
    mapped_file_tests.cpp
)

target_link_libraries(sap_fs_tests
    PRIVATE
        sap::fs
        GTest::gtest_main
)

target_compile_features(sap_fs_tests PRIVATE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sap_fs_tests PRIVATE -Wall -Wextra -Wpedantic)
elseif(MSVC)
    target_compile_options(sap_fs_tests PRIVATE /W4)
endif()

gtest_discover_tests(sap_fs_tests)