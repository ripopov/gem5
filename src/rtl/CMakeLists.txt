cmake_minimum_required(VERSION 3.20)

project(rtl_cosim LANGUAGES CXX)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

option(RTL_COSIM_BUILD_TESTS "Build RTL co-simulation GoogleTests" ON)
option(RTL_COSIM_BUILD_PULP_FIXTURES "Build Verilated PULP fixtures" OFF)
option(RTL_COSIM_BUILD_SCR1 "Build the SCR1 reference vendor DLL" OFF)
option(RTL_COSIM_ENABLE_SANITIZERS "Enable address and undefined sanitizers" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(GEM5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(rtl_cosim_runtime
    runtime/json.cc
    runtime/image_loader.cc
    runtime/model_loader.cc
    runtime/model_validator.cc
    runtime/signal_access.cc
    runtime/transaction.cc
    runtime/protocol/apb.cc
    runtime/protocol/axi.cc
)
add_library(rtl_cosim::runtime ALIAS rtl_cosim_runtime)

add_library(rtl_cosim_checker_support
    checker/checker_config.cc
    checker/memory_backend.cc
)
add_library(rtl_cosim::checker_support ALIAS rtl_cosim_checker_support)

target_include_directories(rtl_cosim_runtime
    PUBLIC
        "${GEM5_ROOT}/include"
        "${GEM5_ROOT}/src"
)
target_compile_features(rtl_cosim_runtime PUBLIC cxx_std_20)
target_link_libraries(rtl_cosim_checker_support PUBLIC rtl_cosim_runtime)
target_compile_features(rtl_cosim_checker_support PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(rtl_cosim_runtime PRIVATE /W4 /permissive-)
    target_compile_options(rtl_cosim_checker_support PRIVATE
        /W4 /permissive-
    )
else()
    target_compile_options(rtl_cosim_runtime PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
    target_compile_options(rtl_cosim_checker_support PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
    target_link_libraries(rtl_cosim_runtime PUBLIC "${CMAKE_DL_LIBS}")
endif()

if(RTL_COSIM_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(rtl_cosim_runtime PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(rtl_cosim_runtime PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
endif()

if(RTL_COSIM_BUILD_TESTS)
    enable_testing()
    if(EXISTS "${GEM5_ROOT}/ext/googletest/CMakeLists.txt")
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            "${GEM5_ROOT}/ext/googletest"
            "${CMAKE_CURRENT_BINARY_DIR}/googletest"
            EXCLUDE_FROM_ALL
        )
    else()
        find_package(GTest 1.12 REQUIRED)
    endif()

    add_library(rtl_cosim_test_vendor SHARED tests/vendor_fixture.cc)
    set_target_properties(rtl_cosim_test_vendor PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    target_include_directories(rtl_cosim_test_vendor PRIVATE
        "${GEM5_ROOT}/include"
    )
    target_compile_features(rtl_cosim_test_vendor PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(rtl_cosim_test_vendor PRIVATE
            /W4 /permissive-
        )
    else()
        target_compile_options(rtl_cosim_test_vendor PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    if(RTL_COSIM_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(rtl_cosim_test_vendor PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(rtl_cosim_test_vendor PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
    endif()

    add_executable(rtl_cosim_tests
        tests/apb.test.cc
        tests/axi.test.cc
        tests/checker_config.test.cc
        tests/image_loader.test.cc
        tests/model_loader.test.cc
        tests/model_validator.test.cc
        tests/transaction.test.cc
    )
    target_link_libraries(rtl_cosim_tests PRIVATE
        rtl_cosim_checker_support GTest::gtest_main
    )
    target_compile_definitions(rtl_cosim_tests PRIVATE
        RTL_COSIM_TEST_VENDOR_PATH="$<TARGET_FILE:rtl_cosim_test_vendor>"
    )
    add_dependencies(rtl_cosim_tests rtl_cosim_test_vendor)
    if(MSVC)
        target_compile_options(rtl_cosim_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(rtl_cosim_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    include(GoogleTest)
    gtest_discover_tests(rtl_cosim_tests
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )
endif()

if(RTL_COSIM_BUILD_PULP_FIXTURES)
    add_subdirectory(fixtures/pulp)
endif()

add_executable(rtl-cosim-check checker/main.cc)
target_link_libraries(rtl-cosim-check PRIVATE rtl_cosim_checker_support)
if(MSVC)
    target_compile_options(rtl-cosim-check PRIVATE /W4 /permissive-)
else()
    target_compile_options(rtl-cosim-check PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

if(RTL_COSIM_BUILD_TESTS)
    set(CHECK_EXIT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_exit.cmake")
    set(TEST_VENDOR_CONFIG
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/config/vendor_smoke.json")
    function(add_checker_exit_test name expected library config)
        add_test(NAME ${name}
            COMMAND "${CMAKE_COMMAND}"
                "-DCHECKER=$<TARGET_FILE:rtl-cosim-check>"
                "-DLIBRARY=${library}"
                "-DCONFIG=${config}"
                "-DEXPECTED=${expected}"
                -P "${CHECK_EXIT_SCRIPT}"
        )
    endfunction()
    add_checker_exit_test(checker_smoke 0
        "$<TARGET_FILE:rtl_cosim_test_vendor>" "${TEST_VENDOR_CONFIG}")
    add_checker_exit_test(checker_invalid_json 1
        "$<TARGET_FILE:rtl_cosim_test_vendor>"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/config/invalid.json")
    add_checker_exit_test(checker_missing_library 2
        "${CMAKE_CURRENT_BINARY_DIR}/missing-vendor-library"
        "${TEST_VENDOR_CONFIG}")
    add_checker_exit_test(checker_timeout 5
        "$<TARGET_FILE:rtl_cosim_test_vendor>"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/config/vendor_timeout.json")
endif()

if(RTL_COSIM_BUILD_SCR1)
    set(RTL_COSIM_SCR1_ENABLE_TESTS ${RTL_COSIM_BUILD_TESTS})
    add_subdirectory("${GEM5_ROOT}/ext/rtl/scr1" scr1)
endif()
