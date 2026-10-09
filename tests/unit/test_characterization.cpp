// Unit tests for the characterization-JSON loader and the Liberty emitter's
// handling of JSON-supplied strings: malformed input must come back as
// bool+error (never an exception or a silently truncated value), and strings
// that reach the .lib must not be able to break its grammar.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "characterization_data.hpp"
#include "liberty_estimator.hpp"

namespace {

namespace fs = std::filesystem;

fs::path scratch_dir() {
    const fs::path dir = fs::temp_directory_path() / "openfinram_char_unit";
    fs::create_directories(dir);
    return dir;
}

fs::path write_file(const std::string& name, const std::string& text) {
    const fs::path p = scratch_dir() / name;
    std::ofstream out(p);
    out << text;
    return p;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::string mock_lef(bool single_port) {
    std::string lef = "MACRO sram_x4x4x1\n  SIZE 10.0 BY 20.0 ;\n";
    auto pin = [&lef](const std::string& name) {
        lef += "  PIN " + name + "\n  END " + name + "\n";
    };
    pin("vdd");
    pin("vss");
    pin("clk");
    if (single_port) {
        for (const char* name : {"ce_n", "oe_n", "we_n"}) pin(name);
        for (int bit = 0; bit < 4; ++bit) pin("sdel[" + std::to_string(bit) + "]");
        for (int bit = 0; bit < 4; ++bit) pin("A[" + std::to_string(bit) + "]");
        for (int bit = 0; bit < 4; ++bit) {
            pin("D[" + std::to_string(bit) + "]");
            pin("Q[" + std::to_string(bit) + "]");
        }
    } else {
        for (const char* name : {"rst_n", "ce_n_A", "ce_n_B", "we_n_A", "we_n_B",
                                 "oe_n_A", "oe_n_B"}) {
            pin(name);
        }
        for (int bit = 0; bit < 4; ++bit) {
            pin("A_A[" + std::to_string(bit) + "]");
            pin("A_B[" + std::to_string(bit) + "]");
        }
        for (int bit = 0; bit < 4; ++bit) {
            pin("D_A[" + std::to_string(bit) + "]");
            pin("D_B[" + std::to_string(bit) + "]");
            pin("Q_A[" + std::to_string(bit) + "]");
            pin("Q_B[" + std::to_string(bit) + "]");
        }
    }
    return lef + "END sram_x4x4x1\n";
}

bool load(const std::string& name, const std::string& json,
          OpenFinRAM::CharacterizationData& data, std::string& err) {
    return OpenFinRAM::load_characterization_json(
        write_file(name, json).string(), data, &err);
}

// Emit the .lib for `json` and return its text.
std::string emit_lib(const std::string& tag, const std::string& json) {
    OpenFinRAM::CharacterizationData data;
    std::string err;
    EXPECT_TRUE(load(tag + ".json", json, data, err)) << err;
    MainCliOptions opts;
    opts.single_port = true;
    opts.num_wls = 2;
    opts.num_data_bits = 4;
    opts.num_banks = 1;
    const fs::path lef = write_file(tag + ".lef", mock_lef(true));
    const fs::path lib = scratch_dir() / (tag + ".lib");
    EXPECT_TRUE(OpenFinRAM::export_estimated_liberty(
        opts, lef.string(), lib.string(), &err, &data)) << err;
    return read_file(lib);
}

constexpr const char* kSchema = R"j("schema": "openfinram-characterization-1")j";

}  // namespace

// --- JSON parser robustness -------------------------------------------------

TEST(CharacterizationJson, MalformedUnicodeEscapeIsAnErrorNotAThrow) {
    OpenFinRAM::CharacterizationData data;
    std::string err;
    bool ok = true;
    EXPECT_NO_THROW(ok = load("bad_u.json",
        std::string("{") + kSchema + R"j(, "comment": "\uZZZZ"})j", data, err));
    EXPECT_FALSE(ok);
    EXPECT_FALSE(err.empty());
}

TEST(CharacterizationJson, ValidUnicodeEscapeDecodes) {
    OpenFinRAM::CharacterizationData data;
    std::string err;
    ASSERT_TRUE(load("good_u.json",
        std::string("{") + kSchema + R"j(, "comment": "\u0041\u00e9"})j", data, err)) << err;
    EXPECT_EQ(data.comment, "A\xC3\xA9");
}

TEST(CharacterizationJson, NumberWithTrailingGarbageIsRejected) {
    OpenFinRAM::CharacterizationData data;
    std::string err;
    EXPECT_FALSE(load("bad_num.json",
        std::string("{") + kSchema + R"j(, "clock_min_period": 1.2.3})j", data, err));
    EXPECT_FALSE(load("bad_exp.json",
        std::string("{") + kSchema + R"j(, "clock_min_period": 1e+e-2})j", data, err));
}

TEST(CharacterizationJson, WellFormedNumbersStillParse) {
    OpenFinRAM::CharacterizationData data;
    std::string err;
    ASSERT_TRUE(load("num.json",
        std::string("{") + kSchema + R"j(, "clock_min_period": -1.5e-1})j", data, err)) << err;
    EXPECT_DOUBLE_EQ(data.clock_min_period, -0.15);
}

TEST(CharacterizationJson, OperatingConditionsNameMustBeAnIdentifier) {
    OpenFinRAM::CharacterizationData data;
    std::string err;
    EXPECT_FALSE(load("bad_oc.json",
        std::string("{") + kSchema +
        R"j(, "operating_conditions": {"voltage": 0.63, "temperature": 25, "name": "SS (0.63V)"}})j",
        data, err));
    EXPECT_NE(err.find("operating_conditions.name"), std::string::npos) << err;
    ASSERT_TRUE(load("good_oc.json",
        std::string("{") + kSchema +
        R"j(, "operating_conditions": {"voltage": 0.63, "temperature": 25, "name": "SS_0P63V_25C"}})j",
        data, err)) << err;
    EXPECT_EQ(data.operating_conditions.name, "SS_0P63V_25C");
}

// --- Liberty emitter ----------------------------------------------------------

TEST(LibertyEstimator, DefaultCornerKeepsHistoricalName) {
    const std::string lib = emit_lib("oc_default",
        std::string("{") + kSchema +
        R"j(, "operating_conditions": {"voltage": 0.7, "temperature": 25}})j");
    EXPECT_NE(lib.find("operating_conditions (PVT_0P7V_25C) {"), std::string::npos);
    EXPECT_NE(lib.find("default_operating_conditions : PVT_0P7V_25C;"), std::string::npos);
}

TEST(LibertyEstimator, NominalVoltageAtOtherTemperatureIsNotNamed25C) {
    const std::string lib = emit_lib("oc_hot",
        std::string("{") + kSchema +
        R"j(, "operating_conditions": {"voltage": 0.7, "temperature": 125}})j");
    EXPECT_EQ(lib.find("PVT_0P7V_25C"), std::string::npos);
    EXPECT_NE(lib.find("operating_conditions (PVT_0P7V_125C) {"), std::string::npos);
    EXPECT_NE(lib.find("temperature : 125;"), std::string::npos);
}

TEST(LibertyEstimator, NegativeTemperatureYieldsValidIdentifier) {
    const std::string lib = emit_lib("oc_cold",
        std::string("{") + kSchema +
        R"j(, "operating_conditions": {"voltage": 0.77, "temperature": -40}})j");
    EXPECT_NE(lib.find("operating_conditions (PVT_0P77V_M40C) {"), std::string::npos);
    EXPECT_NE(lib.find("default_operating_conditions : PVT_0P77V_M40C;"), std::string::npos);
    EXPECT_EQ(lib.find("_-40C"), std::string::npos);
    EXPECT_NE(lib.find("temperature : -40;"), std::string::npos);
}

TEST(LibertyEstimator, CommentQuotesAndBackslashesAreEscaped) {
    const std::string lib = emit_lib("comment",
        std::string("{") + kSchema +
        R"j(, "comment": "path C:\\x \"snapshot\" line1\nline2"})j");
    EXPECT_NE(lib.find(R"j(comment : "path C:\\x \"snapshot\" line1 line2";)j"),
              std::string::npos) << lib.substr(0, 400);
}

TEST(LibertyEstimator, DualPortUsesBothWriteAddressesAndAsyncResetChecks) {
    MainCliOptions opts;
    opts.single_port = false;
    opts.num_wls = 2;
    opts.num_data_bits = 4;
    opts.num_banks = 1;

    const fs::path lef = write_file("dual_port.lef", mock_lef(false));
    const fs::path lib_path = scratch_dir() / "dual_port.lib";
    std::string err;
    ASSERT_TRUE(OpenFinRAM::export_estimated_liberty(
        opts, lef.string(), lib_path.string(), &err)) << err;

    const std::string lib = read_file(lib_path);
    EXPECT_NE(lib.find("bus (A_A)"), std::string::npos);
    EXPECT_NE(lib.find("bus (A_B)"), std::string::npos);
    EXPECT_NE(lib.find("bus (D_A)"), std::string::npos);
    EXPECT_NE(lib.find("bus (D_B)"), std::string::npos);
    EXPECT_NE(lib.find("address : A_A;"), std::string::npos);
    EXPECT_NE(lib.find("address : A_B;"), std::string::npos);
    EXPECT_NE(lib.find("pin (we_n_B)"), std::string::npos);
    EXPECT_EQ(lib.find("address : A;"), std::string::npos);
    EXPECT_NE(lib.find("Same-address concurrent A/B accesses are illegal when either port writes"),
              std::string::npos);
    EXPECT_NE(lib.find("contention_condition : \""), std::string::npos);
    EXPECT_NE(lib.find("(!ce_n_A) * (!ce_n_B)"), std::string::npos);
    EXPECT_NE(lib.find("((!we_n_A) + (!we_n_B))"), std::string::npos);
    for (int bit = 0; bit < 4; ++bit) {
        const std::string equal_bit =
            "((A_A[" + std::to_string(bit) + "] * A_B[" +
            std::to_string(bit) + "]) + ((!A_A[" + std::to_string(bit) +
            "]) * (!A_B[" + std::to_string(bit) + "])))";
        EXPECT_NE(lib.find(equal_bit), std::string::npos) << equal_bit;
    }

    const std::size_t reset_begin = lib.find("pin (rst_n)");
    const std::size_t reset_end = lib.find("pin (ce_n_A)", reset_begin);
    ASSERT_NE(reset_begin, std::string::npos);
    ASSERT_NE(reset_end, std::string::npos);
    const std::string reset = lib.substr(reset_begin, reset_end - reset_begin);
    EXPECT_NE(reset.find("timing_type : recovery_rising;"), std::string::npos);
    EXPECT_NE(reset.find("timing_type : removal_rising;"), std::string::npos);
    EXPECT_EQ(reset.find("timing_type : setup_rising;"), std::string::npos);
    EXPECT_EQ(reset.find("timing_type : hold_rising;"), std::string::npos);
}

TEST(LibertyEstimator, RejectsDualPortLibertyForSinglePortLef) {
    MainCliOptions opts;
    opts.single_port = false;
    opts.num_wls = 2;
    opts.num_data_bits = 4;
    opts.num_banks = 1;

    const fs::path lef = write_file("wrong_interface.lef", mock_lef(true));
    const fs::path lib_path = scratch_dir() / "wrong_interface.lib";
    std::string err;
    EXPECT_FALSE(OpenFinRAM::export_estimated_liberty(
        opts, lef.string(), lib_path.string(), &err));
    EXPECT_NE(err.find("missing PIN rst_n"), std::string::npos) << err;
}
