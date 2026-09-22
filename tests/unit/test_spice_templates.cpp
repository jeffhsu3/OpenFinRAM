#include <gtest/gtest.h>

#include <algorithm>
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
#include "spice_generator.hpp"

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

std::vector<std::string> subckt_body(const std::string& text,
                                     const std::string& name);

size_t subckt_instance_node_count(const std::string& text,
                                  const std::string& subckt,
                                  const std::string& instance) {
    const auto body = subckt_body(text, subckt);
    for (const auto& statement : body) {
        const auto fields = tokens(statement);
        if (!fields.empty() && fields.front() == instance) {
            return fields.size() - 2;
        }
    }
    ADD_FAILURE() << "missing instance " << instance << " in " << subckt;
    return 0;
}

size_t named_subckt_port_count(const std::string& text, const std::string& name) {
    const auto body = subckt_body(text, name);
    if (body.empty()) {
        return 0;
    }
    return tokens(body.front()).size() - 2;
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

std::map<std::string, std::string> instance_bindings(
    const std::string& parent, const std::string& instance,
    const std::string& child) {
    const auto child_statements = logical_statements(child);
    if (child_statements.empty()) {
        ADD_FAILURE() << "empty child subcircuit";
        return {};
    }
    const auto formal = tokens(child_statements.front());
    for (const auto& statement : logical_statements(parent)) {
        const auto actual = tokens(statement);
        if (actual.empty() || actual.front() != instance) {
            continue;
        }
        if (formal.size() != actual.size()) {
            ADD_FAILURE() << instance << " has " << actual.size() - 2
                          << " actual nodes for " << formal.size() - 2
                          << " formal ports";
            return {};
        }
        std::map<std::string, std::string> bindings;
        for (size_t index = 2; index < formal.size(); ++index) {
            bindings.emplace(formal[index], actual[index - 1]);
        }
        return bindings;
    }
    ADD_FAILURE() << "missing instance " << instance;
    return {};
}

// Although SPICE identifiers are case-insensitive, downstream netlist tools
// are not universally so.  Report two spellings of the same logical node
// within any generated subcircuit before that ambiguity can become a split
// net in a case-sensitive consumer.
std::vector<std::string> case_aliased_nets(const std::string& text) {
    std::set<std::string> collisions;
    std::map<std::string, std::string> spelling;
    std::string current;
    auto record = [&](const std::string& node) {
        const std::string key = upper(node);
        const auto found = spelling.find(key);
        if (found == spelling.end()) {
            spelling.emplace(key, node);
        } else if (found->second != node) {
            collisions.insert(current + ": " + found->second + " / " + node);
        }
    };

    for (const auto& statement : logical_statements(text)) {
        const auto fields = tokens(statement);
        if (fields.empty()) {
            continue;
        }
        if (upper(fields.front()) == ".SUBCKT") {
            current = fields.size() > 1 ? fields[1] : "<unnamed>";
            spelling.clear();
            for (size_t index = 2; index < fields.size(); ++index) {
                record(fields[index]);
            }
            continue;
        }
        if (upper(fields.front()) == ".ENDS") {
            current.clear();
            spelling.clear();
            continue;
        }
        if (current.empty()) {
            continue;
        }

        const char kind = static_cast<char>(
            std::toupper(static_cast<unsigned char>(fields.front().front())));
        if (kind == 'M') {
            for (size_t node = 1; node <= 4 && node < fields.size(); ++node) {
                record(fields[node]);
            }
        } else if (kind == 'X') {
            for (size_t node = 1; node + 1 < fields.size(); ++node) {
                record(fields[node]);
            }
        }
    }
    return {collisions.begin(), collisions.end()};
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

namespace {

// The column group of a small generated deck, as one text the binding helpers
// can search without also finding the XIO_* instances inside the wrappers.
std::string generated_colgrp_8t() {
    MainCliOptions config;
    config.single_port = false;
    config.num_wls = 2;
    config.num_data_bits = 4;
    config.num_banks = 1;
    const std::string generated =
        OpenFinRAM::SpiceGenerator(config).generate_spice_content();
    std::string text;
    for (const auto& statement : subckt_body(generated, "colgrp_sram_8t")) {
        text += statement + "\n";
    }
    return text;
}

}  // namespace

TEST(SpiceTemplates8T, ColumnGroupUsesPortWrappersAndSupportsBothWritePorts) {
    const std::string colgrp = generated_colgrp_8t();

    EXPECT_EQ(count_occurrences(colgrp, "ioprech_sram_8t_a"), 1U);
    EXPECT_EQ(count_occurrences(colgrp, "ioprech_sram_8t_b"), 1U);
    EXPECT_EQ(count_occurrences(colgrp, "array_sram_8t"), 1U);  // one unsplit array
    EXPECT_NE(colgrp.find(
        "XIO_B wrenanB wrenaB sae_B sae_B oeb_outB oe_outB DB QB"),
        std::string::npos);
    EXPECT_EQ(colgrp.find("XIO_B VDD VSS"), std::string::npos);
    EXPECT_EQ(colgrp.find("iocolgrp_sram_8t"), std::string::npos);
    for (const std::string& split_era : {"WLTA", "WLBA", "yselt", "yselb", "blprecht"}) {
        EXPECT_EQ(colgrp.find(split_era), std::string::npos) << split_era;
    }
}

TEST(SpiceTemplates8T, WrapperAndColumnGroupInstanceAritiesMatch) {
    const std::string core = OpenFinRAM::SpiceTemplates::get_iocolgrp();
    const std::string port_a = OpenFinRAM::SpiceTemplates::get_ioprech_8t_a();
    const std::string port_b = OpenFinRAM::SpiceTemplates::get_ioprech_8t_b();
    const std::string colgrp = generated_colgrp_8t();

    EXPECT_EQ(instance_node_count(port_a, "XIO_A"), subckt_port_count(core));
    EXPECT_EQ(instance_node_count(port_b, "XIO_B"), subckt_port_count(core));
    EXPECT_EQ(instance_node_count(colgrp, "XIO_A"), subckt_port_count(port_a));
    EXPECT_EQ(instance_node_count(colgrp, "XIO_B"), subckt_port_count(port_b));
}

TEST(SpiceTemplates8T, WriteEnableBindingsFollowWrapperFormalOrder) {
    const std::string port_a = OpenFinRAM::SpiceTemplates::get_ioprech_8t_a();
    const std::string port_b = OpenFinRAM::SpiceTemplates::get_ioprech_8t_b();
    const std::string colgrp = generated_colgrp_8t();

    const auto a = instance_bindings(colgrp, "XIO_A", port_a);
    const auto b = instance_bindings(colgrp, "XIO_B", port_b);
    ASSERT_EQ(a.count("WRENAN_A"), 1U);
    ASSERT_EQ(a.count("WRENA_A"), 1U);
    ASSERT_EQ(b.count("WRENAN_B"), 1U);
    ASSERT_EQ(b.count("WRENA_B"), 1U);
    EXPECT_EQ(a.at("WRENAN_A"), "wrenanA");
    EXPECT_EQ(a.at("WRENA_A"), "wrenaA");
    EXPECT_EQ(b.at("WRENAN_B"), "wrenanB");
    EXPECT_EQ(b.at("WRENA_B"), "wrenaB");
}

// One IO per end of the bitlines: port A stands left of the array and meets
// it with the wrapper's B face, port B stands right and meets it with its T
// face.  The face turned away idles precharged with every column deselected.
TEST(SpiceTemplates8T, EachPortUsesTheFaceTurnedToTheArrayAndIdlesTheOther) {
    const std::string colgrp = generated_colgrp_8t();
    const auto a = instance_bindings(
        colgrp, "XIO_A", OpenFinRAM::SpiceTemplates::get_ioprech_8t_a());
    const auto b = instance_bindings(
        colgrp, "XIO_B", OpenFinRAM::SpiceTemplates::get_ioprech_8t_b());
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());

    EXPECT_EQ(a.at("BLPRECHBN_A"), "blprechnA");
    EXPECT_EQ(a.at("BLPRECHTN_A"), "VSS");
    EXPECT_EQ(b.at("BLPRECHTN_B"), "blprechnB");
    EXPECT_EQ(b.at("BLPRECHBN_B"), "VSS");
    std::set<std::string> idle_stubs;
    for (int i = 0; i < 4; ++i) {
        const std::string n = "[" + std::to_string(i) + "]";
        EXPECT_EQ(a.at("BLB_A" + n), "BL_A" + n);
        EXPECT_EQ(a.at("BLBN_A" + n), "BLN_A" + n);
        EXPECT_EQ(a.at("YSELB_A" + n), "yselA" + n);
        EXPECT_EQ(a.at("YSELBN_A" + n), "yselnA" + n);
        EXPECT_EQ(a.at("YSELT_A" + n), "VSS");
        EXPECT_EQ(a.at("YSELTN_A" + n), "VDD");

        EXPECT_EQ(b.at("BLT_B" + n), "BL_B" + n);
        EXPECT_EQ(b.at("BLTN_B" + n), "BLN_B" + n);
        EXPECT_EQ(b.at("YSELT_B" + n), "yselB" + n);
        EXPECT_EQ(b.at("YSELTN_B" + n), "yselnB" + n);
        EXPECT_EQ(b.at("YSELB_B" + n), "VSS");
        EXPECT_EQ(b.at("YSELBN_B" + n), "VDD");

        for (const std::string& stub : {a.at("BLT_A" + n), a.at("BLTN_A" + n),
                                        b.at("BLB_B" + n), b.at("BLBN_B" + n)}) {
            EXPECT_EQ(stub.rfind("idle_", 0), 0U) << stub;
            idle_stubs.insert(stub);
        }
    }
    EXPECT_EQ(idle_stubs.size(), 16U);  // sixteen separate stubs, none shared

    // The array's bitlines are exactly the ones the two used faces carry.
    const auto array = instance_bindings(
        colgrp, "X0", [] {
            MainCliOptions config;
            config.single_port = false;
            config.num_wls = 2;
            config.num_data_bits = 4;
            config.num_banks = 1;
            const std::string generated =
                OpenFinRAM::SpiceGenerator(config).generate_spice_content();
            std::string text;
            for (const auto& statement : subckt_body(generated, "array_sram_8t")) {
                text += statement + "\n";
            }
            return text;
        }());
    ASSERT_FALSE(array.empty());
    for (int i = 0; i < 4; ++i) {
        const std::string n = "[" + std::to_string(i) + "]";
        EXPECT_EQ(array.at("BLA" + n), "BL_A" + n);
        EXPECT_EQ(array.at("BLAN" + n), "BLN_A" + n);
        EXPECT_EQ(array.at("BLB" + n), "BL_B" + n);
        EXPECT_EQ(array.at("BLBN" + n), "BLN_B" + n);
    }
    EXPECT_EQ(array.count("WLA[3]"), 1U);  // NUM_WL = 2: four wordlines, one array
    EXPECT_EQ(array.count("WLA[4]"), 0U);
}

TEST(SpiceTemplates8T, GeneratedDeckOmitsRetiredReplicaSaeScaffolding) {
    MainCliOptions config;
    config.single_port = false;
    config.num_wls = 2;
    config.num_data_bits = 4;
    config.num_banks = 1;
    const std::string generated =
        OpenFinRAM::SpiceGenerator(config).generate_spice_content();

    EXPECT_EQ(generated.find("buf_sram"), std::string::npos);
    EXPECT_EQ(generated.find("skewed_inv_sram"), std::string::npos);
    EXPECT_EQ(generated.find("replica_cell_8t"), std::string::npos);
    EXPECT_EQ(generated.find("sram_prech_ymux_8t_v1"), std::string::npos);
    EXPECT_EQ(generated.find("sram_prech_ymux_8t_v2"), std::string::npos);
    EXPECT_EQ(generated.find("wrasst_prech_ymux_x8_sram_8t"), std::string::npos);
}

TEST(SpiceTemplates, GeneratedDecksHaveNoCaseAliasedNets) {
    MainCliOptions config;
    config.num_wls = 2;
    config.num_data_bits = 4;
    config.num_banks = 1;

    config.single_port = true;
    const auto single = case_aliased_nets(
        OpenFinRAM::SpiceGenerator(config).generate_spice_content());
    for (const auto& collision : single) {
        ADD_FAILURE() << "single-port " << collision;
    }

    config.single_port = false;
    const auto dual = case_aliased_nets(
        OpenFinRAM::SpiceGenerator(config).generate_spice_content());
    for (const auto& collision : dual) {
        ADD_FAILURE() << "dual-port " << collision;
    }
}

TEST(SpiceTemplates8T, GeneratedMacroHierarchyCarriesBothWritePorts) {
    MainCliOptions config;
    config.single_port = false;
    config.num_wls = 2;
    config.num_data_bits = 4;
    config.num_banks = 1;
    OpenFinRAM::SpiceGenerator generator(config);
    const std::string generated = generator.generate_spice_content();

    const auto colgrp = subckt_body(generated, "colgrp_sram_8t");
    const auto stacked = subckt_body(generated, "stacked_colgrp_x4x2x1");
    ASSERT_FALSE(colgrp.empty());
    ASSERT_FALSE(stacked.empty());
    const auto colgrp_header = tokens(colgrp.front());
    const auto stacked_header = tokens(stacked.front());
    for (const std::string& pin : {"DB", "wrenaB", "wrenanB"}) {
        EXPECT_NE(std::find(colgrp_header.begin(), colgrp_header.end(), pin),
                  colgrp_header.end()) << pin;
    }
    for (const std::string& pin : {"DB[0]", "DB[1]", "wrenaB[0]", "wrenanB[0]"}) {
        EXPECT_NE(std::find(stacked_header.begin(), stacked_header.end(), pin),
                  stacked_header.end()) << pin;
    }
    EXPECT_EQ(subckt_instance_node_count(generated, "colgrp_sram_8t", "X0"),
              named_subckt_port_count(generated, "array_sram_8t"));
    EXPECT_EQ(subckt_instance_node_count(generated, "stacked_colgrp_x4x2x1", "X0_0"),
              named_subckt_port_count(generated, "colgrp_sram_8t"));

    const auto row = subckt_body(generated, "sram_cell_row_8t");
    size_t active_cells = 0;
    for (const auto& statement : row) {
        const auto fields = tokens(statement);
        ASSERT_FALSE(fields.empty());
        if (fields.front().front() != 'X') continue;
        EXPECT_EQ(fields.back(), "sram_cell_8t");
        ++active_cells;
    }
    // NUM_WL = 2: one unsplit array of four wordlines.  Physical end caps and
    // taps have no dummy devices.
    EXPECT_EQ(active_cells, 4U);
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
