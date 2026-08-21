cmake_minimum_required(VERSION 3.22)
project(OpenFinRAM)

set(CMAKE_CXX_STANDARD 17)

# Add gdstk as subdirectory
add_subdirectory(third_party/gdstk)
add_subdirectory(third_party/argparse)

# Include directories
include_directories(include)
include_directories(third_party/plog/include)

# Collect source files
set(SOURCES
    src/main.cpp
    src/layermap.cpp
    src/cell_utils.cpp
    src/spice_generator.cpp
    src/synthesis_manager.cpp
    src/yosys_manager.cpp
    src/yosys_tcl_generator.cpp
    src/spice_converter.cpp
    src/spice_integrator.cpp
    src/spice_include_resolver.cpp
    src/spice_templates.cpp
    src/pex_runner.cpp
    src/capacitance_parser.cpp
    src/spice_simulator.cpp
    src/innovus_tcl_generator.cpp
    src/innovus_manager.cpp
    src/openroad_manager.cpp
    src/openroad_tcl_generator.cpp
    src/filler_generator.cpp
    src/siliconsmart_generator.cpp
    src/siliconsmart_manager.cpp
    src/lvs_runner.cpp
    src/lef_extractor.cpp
    src/liberty_estimator.cpp
    src/main_config_helpers.cpp
    src/main_flow_helpers.cpp
    src/main_layout_helpers.cpp
    src/utils.cpp
    src/layout_generator.cpp
    src/lvs_manager.cpp
    src/lef_manager.cpp
)

# Create main executable
add_executable(OpenFinRAM ${SOURCES})

# Link libraries
find_package(Threads REQUIRED)
target_link_libraries(OpenFinRAM gdstk argparse Threads::Threads)

include(GNUInstallDirs)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/scripts")
configure_file(
    "${CMAKE_SOURCE_DIR}/scripts/def_to_gds.py"
    "${CMAKE_BINARY_DIR}/scripts/def_to_gds.py"
    COPYONLY
)

install(TARGETS OpenFinRAM RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(
    PROGRAMS "${CMAKE_SOURCE_DIR}/scripts/def_to_gds.py"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/OpenFinRAM/scripts"
)

# ---------------------------------------------------------------------------
# Tests (ctest). The harness scripts cd to the repo root themselves, so they
# work from any build directory. External tools are checked inside the
# scripts; missing tools surface as a clear test failure message.
#   tests/run_decode_check.sh : exhaustive RTL decode-correctness (Icarus)
#   tests/run_equiv_check.sh  : post-synthesis RTL<->netlist equivalence (Yosys)
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
    add_subdirectory(tests)

    add_test(
        NAME decode_check
        COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/tests/run_decode_check.sh
    )
    set_tests_properties(decode_check PROPERTIES TIMEOUT 600)

    add_test(
        NAME equiv_check
        COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/tests/run_equiv_check.sh
    )
    set_tests_properties(equiv_check PROPERTIES TIMEOUT 600)

    # Skips (exit 77) when the KLayout Python bindings are unavailable.
    add_test(
        NAME def2gds_check
        COMMAND ${CMAKE_COMMAND} -E env bash ${CMAKE_SOURCE_DIR}/tests/run_def2gds_check.sh
    )
    set_tests_properties(def2gds_check PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()
