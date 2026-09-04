#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "spice_templates.hpp"

namespace {

std::string read_tech_spice(const std::string& name) {
    std::ifstream input(std::string(REPO_ROOT) + "/tech/spice/" + name);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string read_reference() { return read_tech_spice("sram_8t_ioprech.sp"); }

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

std::string upper(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

// Whitespace-normalized statements of one .SUBCKT block, with the optional
// name on .ENDS dropped so the tracked file and the template compare equal.
std::vector<std::string> subckt_body(const std::string& text,
                                     const std::string& name) {
    std::vector<std::string> body;
    bool inside = false;
    for (const auto& statement : logical_statements(text)) {
        const auto fields = tokens(statement);
        if (fields.empty()) {
            continue;
        }
        if (!inside) {
            inside = upper(fields.front()) == ".SUBCKT" && fields.size() >= 2 &&
                     fields[1] == name;
            if (!inside) {
                continue;
            }
        } else if (upper(fields.front()) == ".ENDS") {
            body.emplace_back(".ENDS");
            return body;
        }
        std::string joined;
        for (const auto& field : fields) {
            if (!joined.empty()) {
                joined += " ";
            }
            joined += field;
        }
        body.push_back(joined);
    }
    ADD_FAILURE() << "missing .SUBCKT " << name;
    return {};
}

// A net used by exactly one device terminal and absent from the port list is
// floating: SPICE creates it silently as a subcircuit-local node.
std::vector<std::string> dangling_nets(const std::string& text,
                                       const std::string& name) {
    const auto body = subckt_body(text, name);
    if (body.empty()) {
        return {};
    }
    std::set<std::string> ports;
    const auto header = tokens(body.front());
    for (size_t index = 2; index < header.size(); ++index) {
        ports.insert(upper(header[index]));
    }

    std::map<std::string, size_t> uses;
    for (size_t line = 1; line < body.size(); ++line) {
        const auto fields = tokens(body[line]);
        if (fields.empty() || upper(fields.front()).front() != 'M') {
            continue;
        }
        if (fields.size() < 6) {
            ADD_FAILURE() << "malformed device line: " << body[line];
            continue;
        }
        for (size_t node = 1; node <= 4; ++node) {  // drain, gate, source, bulk
            ++uses[upper(fields[node])];
        }
    }

    std::vector<std::string> dangling;
    for (const auto& entry : uses) {
        if (entry.second == 1 && ports.find(entry.first) == ports.end()) {
            dangling.push_back(entry.first);
        }
    }
    return dangling;
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

TEST(SpiceTemplates8T, TrackedBitcellNetlistMatchesTheTemplateContract) {
    const std::string cell = read_tech_spice("sram_cell_8t.sp");
    const std::string edges = read_tech_spice("sram_cell_8t_edges.sp");
    ASSERT_FALSE(cell.empty());
    ASSERT_FALSE(edges.empty());

    // tech/spice is the schematic the GDS topology check is documented
    // against, so it must not drift from the emitted deck.
    EXPECT_EQ(subckt_body(cell, "sram_cell_8t"),
              subckt_body(OpenFinRAM::SpiceTemplates::get_cell_8t(),
                          "sram_cell_8t"));
    EXPECT_EQ(subckt_body(edges, "dummy_cell_8t"),
              subckt_body(OpenFinRAM::SpiceTemplates::get_dummy_cell_8t(),
                          "dummy_cell_8t"));
}

TEST(SpiceTemplates8T, BitcellFamilyHasNoFloatingNets) {
    const std::vector<std::pair<std::string, std::string>> cells = {
        {OpenFinRAM::SpiceTemplates::get_cell_8t(), "sram_cell_8t"},
        {OpenFinRAM::SpiceTemplates::get_dummy_cell_8t(), "dummy_cell_8t"},
        {OpenFinRAM::SpiceTemplates::get_replica_cell_8t(), "replica_cell_8t"},
    };
    for (const auto& entry : cells) {
        const auto dangling = dangling_nets(entry.first, entry.second);
        std::string joined;
        for (const auto& net : dangling) {
            joined += (joined.empty() ? "" : ", ") + net;
        }
        EXPECT_TRUE(dangling.empty())
            << entry.second << " has floating nets: " << joined;
    }
}

TEST(SpiceTemplates8T, ReplicaDrivesOnlyItsTrueBitlines) {
    const std::string replica = OpenFinRAM::SpiceTemplates::get_replica_cell_8t();
    const auto body = subckt_body(replica, "replica_cell_8t");
    ASSERT_FALSE(body.empty());

    // The unused complement side sits at the forced QB level rather than on an
    // undeclared RBLAN/RBLBN node, so it carries no current and cannot float.
    EXPECT_EQ(replica.find("RBLAN"), std::string::npos);
    EXPECT_EQ(replica.find("RBLBN"), std::string::npos);
    EXPECT_EQ(count_occurrences(replica, "RBLA"), 2U);  // port list + M4
    EXPECT_EQ(count_occurrences(replica, "RBLB"), 2U);  // port list + M6
}
