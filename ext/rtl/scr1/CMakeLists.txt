cmake_minimum_required(VERSION 3.20)

project(rtl_cosim_scr1 LANGUAGES CXX)

option(RTL_COSIM_SCR1_ENABLE_TESTS
    "Build SCR1 adapter tests when included by the RTL co-simulation runtime"
    OFF
)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(GEM5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(SCR1_REPO "${CMAKE_CURRENT_SOURCE_DIR}/repo")
set(SCR1_RTL "${SCR1_REPO}/src")

if(NOT EXISTS "${SCR1_RTL}/top/scr1_top_axi.sv")
    message(FATAL_ERROR
        "Initialize the SCR1 submodule at ext/rtl/scr1/repo before building")
endif()

find_package(verilator HINTS "$ENV{VERILATOR_ROOT}" "${VERILATOR_ROOT}")
if(NOT verilator_FOUND)
    message(FATAL_ERROR "Verilator is required to build the SCR1 vendor DLL")
endif()

file(STRINGS "${SCR1_RTL}/core.files" SCR1_CORE_FILES)
file(STRINGS "${SCR1_RTL}/axi_top.files" SCR1_AXI_FILES)
list(TRANSFORM SCR1_CORE_FILES PREPEND "${SCR1_RTL}/")
list(TRANSFORM SCR1_AXI_FILES PREPEND "${SCR1_RTL}/")

add_library(rtl_cosim_scr1 SHARED src/adapter.cc)
add_library(rtl_cosim::scr1 ALIAS rtl_cosim_scr1)
target_include_directories(rtl_cosim_scr1 PRIVATE "${GEM5_ROOT}/include")
target_compile_features(rtl_cosim_scr1 PRIVATE cxx_std_20)
set_target_properties(rtl_cosim_scr1 PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if(MSVC)
    set_property(SOURCE src/adapter.cc APPEND PROPERTY
        COMPILE_OPTIONS /W4 /permissive-)
else()
    set_property(SOURCE src/adapter.cc APPEND PROPERTY COMPILE_OPTIONS
        -Wall -Wextra
        -Wno-bool-operation -Wno-sign-compare
        -Wno-unused-parameter -Wno-unused-variable)
endif()

if(RTL_COSIM_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(rtl_cosim_scr1 PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
    target_link_options(rtl_cosim_scr1 PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer
    )
endif()

verilate(rtl_cosim_scr1
    PREFIX Vscr1_top_axi
    TOP_MODULE scr1_top_axi
    INCLUDE_DIRS "${SCR1_RTL}/includes"
    SOURCES
        "${SCR1_RTL}/core/pipeline/scr1_tracelog.sv"
        ${SCR1_CORE_FILES}
        ${SCR1_AXI_FILES}
    VERILATOR_ARGS
        --no-timing --assert --public-flat-rw -Wall -Wno-fatal
        -DSCR1_TRGT_SIMULATION
        -DSCR1_CFG_RV32IMC_MAX
        -DSCR1_TRACE_LOG_DIS
        -Wno-ASCRANGE
        -Wno-CASEINCOMPLETE
        -Wno-DECLFILENAME
        -Wno-MULTIDRIVEN
        -Wno-UNOPTFLAT
        -Wno-UNSIGNED
        -Wno-UNUSEDPARAM
        -Wno-UNUSEDSIGNAL
        -Wno-WIDTHEXPAND
        -Wno-WIDTHTRUNC
)

if(RTL_COSIM_SCR1_ENABLE_TESTS)
    if(NOT TARGET rtl_cosim_runtime OR NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR
            "SCR1 tests must be enabled through src/rtl with RTL_COSIM_BUILD_SCR1")
    endif()

    find_program(SCR1_RISCV_GCC
        NAMES riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc)
    if(NOT SCR1_RISCV_GCC)
        message(FATAL_ERROR
            "A RISC-V bare-metal GCC is required for the SCR1 tests")
    endif()

    set(SCR1_PROGRAM_DIR "${CMAKE_CURRENT_BINARY_DIR}/programs")
    set(SCR1_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tests/programs/link.ld")
    set(SCR1_PROGRAM_FLAGS
        -march=rv32imc_zicsr -mabi=ilp32 -mno-relax -msmall-data-limit=0
        -O2 -ffreestanding -fno-builtin -fno-stack-protector
        -Wall -Wextra -Werror
        -nostdlib -nostartfiles
        -Wl,--build-id=none
        -Wl,-T,${SCR1_LINKER_SCRIPT}
    )
    function(add_scr1_program name)
        set(output "${SCR1_PROGRAM_DIR}/${name}.elf")
        set(sources)
        foreach(source IN LISTS ARGN)
            list(APPEND sources
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/programs/${source}")
        endforeach()
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SCR1_PROGRAM_DIR}"
            COMMAND "${SCR1_RISCV_GCC}" ${SCR1_PROGRAM_FLAGS}
                ${sources} -o "${output}"
            DEPENDS
                ${sources}
                "${SCR1_LINKER_SCRIPT}"
            VERBATIM
        )
        add_custom_target(scr1_${name}_program DEPENDS "${output}")
        set(${name}_ELF "${output}" PARENT_SCOPE)
    endfunction()

    add_scr1_program(riscv_unit riscv_unit.S)
    add_scr1_program(baremetal_tcm baremetal_tcm.S)
    add_scr1_program(c_integration crt0.S c_integration.c)

    add_executable(rtl_cosim_scr1_tests tests/adapter.test.cc)
    target_link_libraries(rtl_cosim_scr1_tests PRIVATE
        rtl_cosim_checker_support GTest::gtest_main
    )
    target_compile_definitions(rtl_cosim_scr1_tests PRIVATE
        "RTL_COSIM_SCR1_PATH=\"$<TARGET_FILE:rtl_cosim_scr1>\""
        "RTL_COSIM_SCR1_BAREMETAL_ELF=\"${baremetal_tcm_ELF}\""
    )
    add_dependencies(rtl_cosim_scr1_tests
        rtl_cosim_scr1 scr1_baremetal_tcm_program)
    if(MSVC)
        target_compile_options(rtl_cosim_scr1_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(rtl_cosim_scr1_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
    include(GoogleTest)
    gtest_discover_tests(rtl_cosim_scr1_tests
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )

    set(SCR1_UNIT_ELF "${riscv_unit_ELF}")
    set(SCR1_BAREMETAL_ELF "${baremetal_tcm_ELF}")
    set(SCR1_C_INTEGRATION_ELF "${c_integration_ELF}")
    configure_file(tests/config/riscv_unit.json.in
        "${CMAKE_CURRENT_BINARY_DIR}/riscv_unit.json" @ONLY)
    configure_file(tests/config/baremetal_tcm.json.in
        "${CMAKE_CURRENT_BINARY_DIR}/baremetal_tcm.json" @ONLY)
    configure_file(tests/config/c_integration.json.in
        "${CMAKE_CURRENT_BINARY_DIR}/c_integration.json" @ONLY)

    add_test(NAME scr1_checker_riscv_unit
        COMMAND rtl-cosim-check "$<TARGET_FILE:rtl_cosim_scr1>"
            "${CMAKE_CURRENT_BINARY_DIR}/riscv_unit.json")
    add_test(NAME scr1_checker_baremetal_tcm
        COMMAND rtl-cosim-check "$<TARGET_FILE:rtl_cosim_scr1>"
            "${CMAKE_CURRENT_BINARY_DIR}/baremetal_tcm.json")
    add_test(NAME scr1_checker_c_integration
        COMMAND rtl-cosim-check "$<TARGET_FILE:rtl_cosim_scr1>"
            "${CMAKE_CURRENT_BINARY_DIR}/c_integration.json")
    set_tests_properties(
        scr1_checker_riscv_unit
        scr1_checker_baremetal_tcm
        scr1_checker_c_integration
        PROPERTIES
            PASS_REGULAR_EXPRESSION "stop: idle after"
            TIMEOUT 60
    )
    add_dependencies(rtl-cosim-check
        scr1_riscv_unit_program
        scr1_baremetal_tcm_program
        scr1_c_integration_program)
endif()
