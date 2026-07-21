find_package(verilator HINTS "$ENV{VERILATOR_ROOT}" "${VERILATOR_ROOT}")
if(NOT verilator_FOUND)
    message(FATAL_ERROR
        "Verilator is required for RTL_COSIM_BUILD_PULP_FIXTURES")
endif()

set(PULP_ROOT "${GEM5_ROOT}/ext/rtl/pulp")
set(PULP_APB "${PULP_ROOT}/apb")
set(PULP_AXI "${PULP_ROOT}/axi")
if(NOT EXISTS "${PULP_APB}/src/apb_pkg.sv" OR
   NOT EXISTS "${PULP_AXI}/src/axi_pkg.sv")
    message(FATAL_ERROR
        "Initialize the pinned PULP AXI and APB submodules before building fixtures")
endif()

function(add_pulp_fixture target definition top)
    add_library(${target} SHARED fixture_adapter.cc)
    target_compile_definitions(${target} PRIVATE ${definition})
    target_include_directories(${target} PRIVATE "${GEM5_ROOT}/include")
    target_compile_features(${target} PRIVATE cxx_std_20)
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-sign-compare -Wno-unused-parameter -Wno-unused-variable
            -Wno-pedantic -Wno-format
        )
    endif()
    if(RTL_COSIM_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
    endif()
    set_property(TARGET ${target} PROPERTY RTL_COSIM_TOP ${top})
endfunction()

add_pulp_fixture(rtl_cosim_pulp_apb_master
    RTL_COSIM_APB_MASTER_FIXTURE apb_master_fixture)
verilate(rtl_cosim_pulp_apb_master
    PREFIX Vapb_master_fixture
    TOP_MODULE apb_master_fixture
    INCLUDE_DIRS "${PULP_APB}/include"
    SOURCES
        "${PULP_APB}/src/apb_pkg.sv"
        "${PULP_APB}/src/apb_demux.sv"
        "${CMAKE_CURRENT_SOURCE_DIR}/rtl/apb_master_fixture.sv"
    VERILATOR_ARGS --timing --assert -Wall
        -Wno-DECLFILENAME -Wno-GENUNNAMED
        -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND
)

add_pulp_fixture(rtl_cosim_pulp_apb_slave
    RTL_COSIM_APB_SLAVE_FIXTURE apb_slave_fixture)
verilate(rtl_cosim_pulp_apb_slave
    PREFIX Vapb_slave_fixture
    TOP_MODULE apb_slave_fixture
    INCLUDE_DIRS "${PULP_APB}/include"
    SOURCES
        "${PULP_APB}/src/apb_pkg.sv"
        "${PULP_APB}/src/apb_demux.sv"
        "${CMAKE_CURRENT_SOURCE_DIR}/rtl/apb_slave_fixture.sv"
    VERILATOR_ARGS --timing --assert -Wall
        -Wno-DECLFILENAME -Wno-GENUNNAMED
        -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND
)

add_pulp_fixture(rtl_cosim_pulp_axi_master
    RTL_COSIM_AXI_MASTER_FIXTURE axi_master_fixture)
verilate(rtl_cosim_pulp_axi_master
    PREFIX Vaxi_master_fixture
    TOP_MODULE axi_master_fixture
    INCLUDE_DIRS "${PULP_AXI}/include"
    SOURCES
        "${PULP_AXI}/src/axi_pkg.sv"
        "${PULP_AXI}/src/axi_modify_address.sv"
        "${CMAKE_CURRENT_SOURCE_DIR}/rtl/axi_master_fixture.sv"
    VERILATOR_ARGS --timing --assert -Wall
        -Wno-DECLFILENAME -Wno-GENUNNAMED
        -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND
)

add_pulp_fixture(rtl_cosim_pulp_axi_slave
    RTL_COSIM_AXI_SLAVE_FIXTURE axi_slave_fixture)
verilate(rtl_cosim_pulp_axi_slave
    PREFIX Vaxi_slave_fixture
    TOP_MODULE axi_slave_fixture
    INCLUDE_DIRS "${PULP_AXI}/include"
    SOURCES
        "${PULP_AXI}/src/axi_pkg.sv"
        "${PULP_AXI}/src/axi_sim_mem.sv"
        "${CMAKE_CURRENT_SOURCE_DIR}/rtl/axi_slave_fixture.sv"
    VERILATOR_ARGS --timing --assert -Wall
        -Wno-DECLFILENAME -Wno-GENUNNAMED
        -Wno-UNUSEDPARAM -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND
        -Wno-WIDTHTRUNC -Wno-INITIALDLY -Wno-PINCONNECTEMPTY
)

if(RTL_COSIM_BUILD_TESTS)
    foreach(kind IN ITEMS apb_master apb_slave axi_master axi_slave)
        add_test(NAME pulp_${kind}_checker
            COMMAND rtl-cosim-check
                "$<TARGET_FILE:rtl_cosim_pulp_${kind}>"
                "${CMAKE_CURRENT_SOURCE_DIR}/config/${kind}.json"
        )
        set_tests_properties(pulp_${kind}_checker PROPERTIES TIMEOUT 60)
    endforeach()

    add_executable(rtl_cosim_pulp_loop_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/pulp_loop.test.cc"
    )
    target_link_libraries(rtl_cosim_pulp_loop_tests PRIVATE
        rtl_cosim_runtime GTest::gtest_main
    )
    target_compile_definitions(rtl_cosim_pulp_loop_tests PRIVATE
        "RTL_COSIM_PULP_APB_MASTER_PATH=\"$<TARGET_FILE:rtl_cosim_pulp_apb_master>\""
        "RTL_COSIM_PULP_APB_SLAVE_PATH=\"$<TARGET_FILE:rtl_cosim_pulp_apb_slave>\""
        "RTL_COSIM_PULP_AXI_MASTER_PATH=\"$<TARGET_FILE:rtl_cosim_pulp_axi_master>\""
        "RTL_COSIM_PULP_AXI_SLAVE_PATH=\"$<TARGET_FILE:rtl_cosim_pulp_axi_slave>\""
    )
    add_dependencies(rtl_cosim_pulp_loop_tests
        rtl_cosim_pulp_apb_master rtl_cosim_pulp_apb_slave
        rtl_cosim_pulp_axi_master rtl_cosim_pulp_axi_slave
    )
    if(MSVC)
        target_compile_options(rtl_cosim_pulp_loop_tests PRIVATE
            /W4 /permissive-
        )
    else()
        target_compile_options(rtl_cosim_pulp_loop_tests PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    include(GoogleTest)
    gtest_discover_tests(rtl_cosim_pulp_loop_tests
        DISCOVERY_TIMEOUT 60
        DISCOVERY_MODE PRE_TEST
    )
endif()
