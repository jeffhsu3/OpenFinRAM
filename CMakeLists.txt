cmake_minimum_required(VERSION 3.22)
project(OpenFinRAM)

set(CMAKE_CXX_STANDARD 11)

# Add gdstk as subdirectory
add_subdirectory(third_party/gdstk)

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
    src/spice_converter.cpp
    src/spice_integrator.cpp
    src/spice_include_resolver.cpp
    src/pex_runner.cpp
    src/capacitance_parser.cpp
    src/spice_simulator.cpp
    src/innovus_tcl_generator.cpp
    src/filler_generator.cpp
    src/siliconsmart_generator.cpp
    src/lvs_runner.cpp
    src/lef_extractor.cpp
    src/main_config_helpers.cpp
    src/main_flow_helpers.cpp
    src/main_layout_helpers.cpp
    src/utils.cpp
)

# Create main executable
add_executable(OpenFinRAM ${SOURCES})

# Link libraries
target_link_libraries(OpenFinRAM gdstk)

# Create GDS comparator executable
add_executable(gds_comparator src/gds_comparator.cpp)
target_link_libraries(gds_comparator gdstk)
target_include_directories(gds_comparator PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_custom_command(TARGET OpenFinRAM POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:OpenFinRAM>/tech
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/tech $<TARGET_FILE_DIR:OpenFinRAM>/tech
)

add_custom_target(verify_golden
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/verify_golden.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    DEPENDS OpenFinRAM gds_comparator
    COMMENT "Run two-case golden regression checks"
    USES_TERMINAL
)
