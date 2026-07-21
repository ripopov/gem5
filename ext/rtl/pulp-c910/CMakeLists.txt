cmake_minimum_required(VERSION 3.20)

project(rtl_cosim_pulp_c910 LANGUAGES CXX)

option(RTL_COSIM_PULP_C910_ENABLE_TESTS
    "Build PULP C910 tests through the RTL co-simulation runtime"
    OFF
)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(GEM5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(C910_REPO "${CMAKE_CURRENT_SOURCE_DIR}/repo")
set(PULP_ROOT "${GEM5_ROOT}/ext/rtl/pulp")
set(PULP_AXI "${PULP_ROOT}/axi")
set(COMMON_CELLS "${PULP_ROOT}/common_cells")
set(TECH_CELLS "${PULP_ROOT}/tech_cells_generic")

if(NOT EXISTS "${C910_REPO}/hw/c910_axi_wrap.sv")
    message(FATAL_ERROR
        "Initialize the PULP C910 submodule at ext/rtl/pulp-c910/repo")
endif()
if(NOT EXISTS "${PULP_AXI}/src/axi_pkg.sv" OR
   NOT EXISTS "${COMMON_CELLS}/src/cf_math_pkg.sv" OR
   NOT EXISTS "${TECH_CELLS}/src/rtl/tc_sram.sv")
    message(FATAL_ERROR
        "Initialize the pinned PULP AXI, common_cells, and tech_cells_generic submodules")
endif()

find_package(verilator HINTS "$ENV{VERILATOR_ROOT}" "${VERILATOR_ROOT}")
if(NOT verilator_FOUND)
    message(FATAL_ERROR "Verilator is required to build the PULP C910 DLL")
endif()

# Preserve the ordered upstream source manifest without requiring Bender in a
# vendor build. Only physical RTL entries are selected; dependency RTL is
# pinned independently under ext/rtl/pulp.
file(STRINGS "${C910_REPO}/Bender.yml" C910_MANIFEST_LINES
    REGEX "^    - (vendor|hw)/.*\\.(v|sv)$")
set(C910_SOURCES)
foreach(line IN LISTS C910_MANIFEST_LINES)
    string(REGEX REPLACE "^    - " "" relative "${line}")
    list(APPEND C910_SOURCES "${C910_REPO}/${relative}")
endforeach()

# The pinned PULP manifest omits BUFGCE from its ordinary source list because
# it is nested under a target selector. It is required for Verilator builds.
list(APPEND C910_SOURCES
    "${C910_REPO}/vendor/thead_openc910/C910_RTL_FACTORY/gen_rtl/common/rtl/BUFGCE.v")

# PULP C910 commit 896e5d3 parameterized two internal LFB vectors but missed
# the corresponding non-ANSI input declarations. Generate a guarded build
# copy rather than modifying the pinned submodule or carrying the full file.
set(C910_LSU_CTRL
    "${C910_REPO}/vendor/thead_openc910/C910_RTL_FACTORY/gen_rtl/lsu/rtl/ct_lsu_ctrl.v")
file(READ "${C910_LSU_CTRL}" C910_LSU_CTRL_TEXT)
set(C910_LSU_CTRL_PATCHED "${C910_LSU_CTRL_TEXT}")
string(REPLACE
    "input   [1  :0]  lsu_had_lfb_data_entry_last;"
    "input   [LFB_DATA_ENTRY-1 :0]  lsu_had_lfb_data_entry_last;"
    C910_LSU_CTRL_PATCHED "${C910_LSU_CTRL_PATCHED}")
string(REPLACE
    "input   [1  :0]  lsu_had_lfb_data_entry_vld;"
    "input   [LFB_DATA_ENTRY-1 :0]  lsu_had_lfb_data_entry_vld;"
    C910_LSU_CTRL_PATCHED "${C910_LSU_CTRL_PATCHED}")
if(C910_LSU_CTRL_PATCHED STREQUAL C910_LSU_CTRL_TEXT)
    message(FATAL_ERROR "The guarded PULP C910 LFB compatibility fix no longer applies")
endif()
set(C910_LSU_CTRL_BUILD "${CMAKE_CURRENT_BINARY_DIR}/ct_lsu_ctrl.v")
file(CONFIGURE OUTPUT "${C910_LSU_CTRL_BUILD}"
    CONTENT "${C910_LSU_CTRL_PATCHED}" @ONLY)
list(REMOVE_ITEM C910_SOURCES "${C910_LSU_CTRL}")
list(APPEND C910_SOURCES "${C910_LSU_CTRL_BUILD}")

set(COMMON_CELL_SOURCES
    "${COMMON_CELLS}/src/cf_math_pkg.sv"
    "${COMMON_CELLS}/src/delta_counter.sv"
    "${COMMON_CELLS}/src/lzc.sv"
    "${COMMON_CELLS}/src/onehot_to_bin.sv"
    "${COMMON_CELLS}/src/spill_register_flushable.sv"
    "${COMMON_CELLS}/src/spill_register.sv"
    "${COMMON_CELLS}/src/counter.sv"
    "${COMMON_CELLS}/src/id_queue.sv"
    "${COMMON_CELLS}/src/rr_arb_tree.sv"
    "${COMMON_CELLS}/src/fifo_v3.sv"
    "${COMMON_CELLS}/src/stream_mux.sv"
    "${COMMON_CELLS}/src/stream_fork.sv"
    "${COMMON_CELLS}/src/stream_fifo.sv"
    "${COMMON_CELLS}/src/stream_fork_dynamic.sv"
    "${COMMON_CELLS}/src/stream_join_dynamic.sv"
    "${COMMON_CELLS}/src/stream_join.sv"
    "${COMMON_CELLS}/src/mem_to_banks_detailed.sv"
)

set(TECH_CELL_SOURCES
    "${TECH_CELLS}/src/rtl/tc_sram.sv"
    "${TECH_CELLS}/src/rtl/tc_sram_impl.sv"
)

set(AXI_SOURCES
    "${PULP_AXI}/src/axi_pkg.sv"
    "${PULP_AXI}/src/axi_demux_id_counters.sv"
    "${PULP_AXI}/src/axi_cut.sv"
    "${PULP_AXI}/src/axi_demux_simple.sv"
    "${PULP_AXI}/src/axi_demux.sv"
    "${PULP_AXI}/src/axi_err_slv.sv"
    "${PULP_AXI}/src/axi_burst_unwrap.sv"
    "${PULP_AXI}/src/axi_to_detailed_mem.sv"
    "${PULP_AXI}/src/axi_zero_mem.sv"
)

add_library(rtl_cosim_pulp_c910 SHARED src/adapter.cc)
add_library(rtl_cosim::pulp_c910 ALIAS rtl_cosim_pulp_c910)
target_include_directories(rtl_cosim_pulp_c910 PRIVATE "${GEM5_ROOT}/include")
target_compile_features(rtl_cosim_pulp_c910 PRIVATE cxx_std_20)
set_target_properties(rtl_cosim_pulp_c910 PROPERTIES
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
    target_compile_options(rtl_cosim_pulp_c910 PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(rtl_cosim_pulp_c910 PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

verilate(rtl_cosim_pulp_c910
    PREFIX Vrtl_cosim_c910_top
    TOP_MODULE rtl_cosim_c910_top
    INCLUDE_DIRS
        "${COMMON_CELLS}/include"
        "${PULP_AXI}/include"
        "${C910_REPO}/vendor/thead_openc910/C910_RTL_FACTORY/gen_rtl/cpu/rtl"
        "${C910_REPO}/vendor/thead_openc910/C910_RTL_FACTORY/gen_rtl/mmu/rtl"
    SOURCES
        ${TECH_CELL_SOURCES}
        ${COMMON_CELL_SOURCES}
        ${AXI_SOURCES}
        ${C910_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/rtl/rtl_cosim_c910_top.sv"
    VERILATOR_ARGS
        --no-timing --assert -Wall -Wno-fatal
        -Wno-ALWCOMBORDER
        -Wno-ASSIGNDLY
        -Wno-CASEINCOMPLETE
        -Wno-CASEOVERLAP
        -Wno-DECLFILENAME
        -Wno-EOFNEWLINE
        -Wno-GENUNNAMED
        -Wno-IMPLICIT
        -Wno-LATCH
        -Wno-MODDUP
        -Wno-MULTIDRIVEN
        -Wno-PINCONNECTEMPTY
        -Wno-PINMISSING
        -Wno-SELRANGE
        -Wno-SPLITVAR
        -Wno-SYNCASYNCNET
        -Wno-UNOPTFLAT
        -Wno-UNSIGNED
        -Wno-UNUSEDGENVAR
        -Wno-UNUSEDPARAM
        -Wno-UNUSEDSIGNAL
        -Wno-WIDTHEXPAND
        -Wno-WIDTHTRUNC
)

if(RTL_COSIM_PULP_C910_ENABLE_TESTS)
    if(NOT TARGET rtl_cosim_runtime OR NOT TARGET GTest::gtest_main)
        message(FATAL_ERROR
            "C910 tests must be enabled through src/rtl with RTL_COSIM_BUILD_PULP_C910")
    endif()
    find_program(C910_RISCV_GCC NAMES riscv64-unknown-elf-gcc)
    if(NOT C910_RISCV_GCC)
        message(FATAL_ERROR
            "A 64-bit RISC-V bare-metal GCC is required for C910 tests")
    endif()

    set(C910_PROGRAM_DIR "${CMAKE_CURRENT_BINARY_DIR}/programs")
    set(C910_LINKER_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/programs/link.ld")
    set(C910_PROGRAM_FLAGS
        -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -mcmodel=medany
        -mno-relax -msmall-data-limit=0
        -O2 -ffreestanding -fno-builtin -fno-stack-protector
        -Wall -Wextra -Werror
        -nostdlib -nostartfiles
        -Wl,--build-id=none
        -Wl,-T,${C910_LINKER_SCRIPT}
    )
    function(add_c910_program name)
        set(output "${C910_PROGRAM_DIR}/${name}.elf")
        set(sources)
        foreach(source IN LISTS ARGN)
            list(APPEND sources
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/programs/${source}")
        endforeach()
        add_custom_command(
            OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${C910_PROGRAM_DIR}"
            COMMAND "${C910_RISCV_GCC}" ${C910_PROGRAM_FLAGS}
                ${sources} -o "${output}"
            DEPENDS ${sources} "${C910_LINKER_SCRIPT}"
            VERBATIM
        )
        add_custom_target(c910_${name}_program DEPENDS "${output}")
        set(${name}_ELF "${output}" PARENT_SCOPE)
    endfunction()

    add_c910_program(boot_wfi boot_wfi.S)
    add_c910_program(error_trap error_trap.S)
    add_c910_program(interrupt_wfi interrupt_wfi.S)
    add_c910_program(c_integration crt0.S c_integration.c)

    add_executable(rtl_cosim_pulp_c910_tests tests/adapter.test.cc)
    target_link_libraries(rtl_cosim_pulp_c910_tests PRIVATE
        rtl_cosim_checker_support GTest::gtest_main)
    target_compile_definitions(rtl_cosim_pulp_c910_tests PRIVATE
        "RTL_COSIM_PULP_C910_PATH=\"$<TARGET_FILE:rtl_cosim_pulp_c910>\""
        "RTL_COSIM_PULP_C910_BOOT_WFI_ELF=\"${boot_wfi_ELF}\""
        "RTL_COSIM_PULP_C910_ERROR_TRAP_ELF=\"${error_trap_ELF}\""
        "RTL_COSIM_PULP_C910_C_INTEGRATION_ELF=\"${c_integration_ELF}\""
        "RTL_COSIM_PULP_C910_INTERRUPT_WFI_ELF=\"${interrupt_wfi_ELF}\"")
    add_dependencies(rtl_cosim_pulp_c910_tests
        rtl_cosim_pulp_c910 c910_boot_wfi_program
        c910_error_trap_program c910_c_integration_program
        c910_interrupt_wfi_program)
    if(MSVC)
        target_compile_options(rtl_cosim_pulp_c910_tests PRIVATE
            /W4 /permissive-)
    else()
        target_compile_options(rtl_cosim_pulp_c910_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
    include(GoogleTest)
    gtest_discover_tests(rtl_cosim_pulp_c910_tests
        DISCOVERY_TIMEOUT 120
        DISCOVERY_MODE PRE_TEST)

    set(C910_BOOT_WFI_ELF "${boot_wfi_ELF}")
    set(C910_ERROR_TRAP_ELF "${error_trap_ELF}")
    set(C910_C_INTEGRATION_ELF "${c_integration_ELF}")
    configure_file(tests/config/boot_wfi.json.in
        "${CMAKE_CURRENT_BINARY_DIR}/boot_wfi.json" @ONLY)
    configure_file(tests/config/error_trap.json.in
        "${CMAKE_CURRENT_BINARY_DIR}/error_trap.json" @ONLY)
    configure_file(tests/config/c_integration.json.in
        "${CMAKE_CURRENT_BINARY_DIR}/c_integration.json" @ONLY)

    add_test(NAME pulp_c910_checker_boot_wfi
        COMMAND rtl-cosim-check "$<TARGET_FILE:rtl_cosim_pulp_c910>"
            "${CMAKE_CURRENT_BINARY_DIR}/boot_wfi.json")
    add_test(NAME pulp_c910_checker_error_trap
        COMMAND rtl-cosim-check "$<TARGET_FILE:rtl_cosim_pulp_c910>"
            "${CMAKE_CURRENT_BINARY_DIR}/error_trap.json")
    add_test(NAME pulp_c910_checker_c_integration
        COMMAND rtl-cosim-check "$<TARGET_FILE:rtl_cosim_pulp_c910>"
            "${CMAKE_CURRENT_BINARY_DIR}/c_integration.json")
    set_tests_properties(
        pulp_c910_checker_boot_wfi
        pulp_c910_checker_error_trap
        pulp_c910_checker_c_integration
        PROPERTIES
            PASS_REGULAR_EXPRESSION "stop: idle after"
            TIMEOUT 300)
    add_dependencies(rtl-cosim-check
        c910_boot_wfi_program c910_error_trap_program
        c910_c_integration_program)
endif()
