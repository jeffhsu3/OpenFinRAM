#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "spice_templates.hpp"

namespace {

std::string read_reference() {
    std::ifstream input(std::string(REPO_ROOT) + "/tech/spice/sram_8t_ioprech.sp");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

size_t count_occurrences(const std::string& text, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

std::vector<std::string> logical_statements(const std::string& text) {
    std::vector<std::string> statements;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() == '+') {
            if (statements.empty()) {
                ADD_FAILURE() << "orphan SPICE continuation line";
                return {};
            }
            statements.back() += " " + line.substr(1);
        } else if (!line.empty() && line.front() != '*') {
            statements.push_back(line);
        }
    }
    return statements;
}

std::vector<std::string> tokens(const std::string& statement) {
    std::istringstream input(statement);
    return {std::istream_iterator<std::string>(input),
            std::istream_iterator<std::string>()};
}

size_t subckt_port_count(const std::string& text) {
    const auto statements = logical_statements(text);
    const auto fields = tokens(statements.front());
    EXPECT_EQ(fields.front(), ".SUBCKT");
    return fields.size() - 2;
}

size_t instance_node_count(const std::string& text, const std::string& name) {
    for (const auto& statement : logical_statements(text)) {
        const auto fields = tokens(statement);
        if (!fields.empty() && fields.front() == name) {
            return fields.size() - 2;  // instance name and referenced subcircuit
        }
    }
    ADD_FAILURE() << "missing instance " << name;
    return 0;
}

}  // namespace

TEST(SpiceTemplates8T, PortWrappersPreserveIndependentControls) {
    const std::string port_a = OpenFinRAM::SpiceTemplates::get_ioprech_8t_a();
    const std::string port_b = OpenFinRAM::SpiceTemplates::get_ioprech_8t_b();

    EXPECT_NE(port_a.find(".SUBCKT ioprech_sram_8t_a"), std::string::npos);
    EXPECT_NE(port_a.find("WRENAN_A WRENA_A SAE_A SAPRECHN_A"), std::string::npos);
    EXPECT_NE(port_a.find("BLTN_A[0] BLTN_A[1] BLTN_A[2] BLTN_A[3]"),
              std::string::npos);
    EXPECT_NE(port_a.find("BLPRECHTN_A BLPRECHBN_A"), std::string::npos);
    EXPECT_EQ(count_occurrences(port_a, "iocolgrp_sram_6t122_v2"), 1U);

    EXPECT_NE(port_b.find(".SUBCKT ioprech_sram_8t_b"), std::string::npos);
    EXPECT_NE(port_b.find("WRENAN_B WRENA_B SAE_B SAPRECHN_B"), std::string::npos);
    EXPECT_NE(port_b.find("BLTN_B[0] BLTN_B[1] BLTN_B[2] BLTN_B[3]"),
              std::string::npos);
    EXPECT_NE(port_b.find("BLPRECHTN_B BLPRECHBN_B"), std::string::npos);
    EXPECT_EQ(count_occurrences(port_b, "iocolgrp_sram_6t122_v2"), 1U);
}

TEST(SpiceTemplates8T, CombinedIoUsesPortWrappersAndDisablesPortBWrite) {
    const std::string combined = OpenFinRAM::SpiceTemplates::get_iocolgrp_8t();

    EXPECT_EQ(count_occurrences(combined, "ioprech_sram_8t_a"), 1U);
    EXPECT_EQ(count_occurrences(combined, "ioprech_sram_8t_b"), 1U);
    EXPECT_NE(combined.find(
        "XIO_B vdd vss sae_B sae_B oeb_out_B oe_out_B vss QB"),
        std::string::npos);
    EXPECT_EQ(combined.find("XSA_A"), std::string::npos);
    EXPECT_EQ(combined.find("XWRMUX_T"), std::string::npos);
}

TEST(SpiceTemplates8T, WrapperAndCompositeInstanceAritiesMatch) {
    const std::string core = OpenFinRAM::SpiceTemplates::get_iocolgrp();
    const std::string port_a = OpenFinRAM::SpiceTemplates::get_ioprech_8t_a();
    const std::string port_b = OpenFinRAM::SpiceTemplates::get_ioprech_8t_b();
    const std::string combined = OpenFinRAM::SpiceTemplates::get_iocolgrp_8t();

    EXPECT_EQ(instance_node_count(port_a, "XIO_A"), subckt_port_count(core));
    EXPECT_EQ(instance_node_count(port_b, "XIO_B"), subckt_port_count(core));
    EXPECT_EQ(instance_node_count(combined, "XIO_A"), subckt_port_count(port_a));
    EXPECT_EQ(instance_node_count(combined, "XIO_B"), subckt_port_count(port_b));
}

TEST(SpiceTemplates8T, TrackedWrapperNetlistMatchesTheTemplateContract) {
    const std::string reference = read_reference();
    ASSERT_FALSE(reference.empty());

    for (const std::string& token : {
             ".SUBCKT ioprech_sram_8t_a",
             ".SUBCKT ioprech_sram_8t_b",
             "XIO_A WRENAN_A WRENA_A SAE_A SAPRECHN_A",
             "XIO_B WRENAN_B WRENA_B SAE_B SAPRECHN_B",
             "BLTN_A[0] BLTN_A[1] BLTN_A[2] BLTN_A[3]",
             "BLTN_B[0] BLTN_B[1] BLTN_B[2] BLTN_B[3]",
         }) {
        EXPECT_NE(reference.find(token), std::string::npos) << token;
    }
    EXPECT_EQ(count_occurrences(reference, "VDD VSS iocolgrp_sram_6t122_v2"), 2U);
}
