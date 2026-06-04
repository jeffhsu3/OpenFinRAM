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
    src/spice_converter.cpp
    src/spice_integrator.cpp
    src/spice_include_resolver.cpp
    src/spice_templates.cpp
    src/pex_runner.cpp
    src/capacitance_parser.cpp
    src/spice_simulator.cpp
    src/innovus_tcl_generator.cpp
    src/innovus_manager.cpp
    src/filler_generator.cpp
    src/siliconsmart_generator.cpp
    src/siliconsmart_manager.cpp
    src/lvs_runner.cpp
    src/lef_extractor.cpp
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
