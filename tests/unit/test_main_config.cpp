#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "main_config_helpers.hpp"

namespace {
MainCliOptions parse(std::vector<std::string> args) {
    args.insert(args.begin(), "OpenFinRAM");
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return parseMainCliOptions(static_cast<int>(argv.size()), argv.data());
}
}

TEST(MainConfig, EightTIntegratesAllBankColumnsByDefault) {
    EXPECT_TRUE(parse({"--openroad"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--num-banks", "2"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--num-banks", "4"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--num-banks", "2", "--share-port-b"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--num-banks", "4", "--share-port-b"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--strips-in-controller"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--num-banks", "4", "--strips-in-controller"}).use_controller_band());
}

TEST(MainConfig, SelectBuffersComeOnWhenOneOutputCannotDriveEverySlice) {
    // 16 kbit in one segment a stack: 2 strips x 64 slices, 171 fF, as built before.
    EXPECT_FALSE(parse({"--openroad", "--bitcell", "6t", "--num-wls", "128", "--num-data-bits", "16"}).select_buffers);
    // 32 kbit: four strips' worth of slices on each sel_lo line.
    const std::vector<std::string> big = {"--openroad", "--bitcell", "6t", "--num-wls", "128",
                                          "--num-data-bits", "32", "--segment-bits", "8"};
    EXPECT_TRUE(parse(big).select_buffers);
    auto forced = big;
    forced.insert(forced.end(), {"--select-buffers", "off"});
    EXPECT_FALSE(parse(forced).select_buffers);
    EXPECT_TRUE(parse({"--openroad", "--bitcell", "6t", "--num-wls", "32", "--num-data-bits", "8",
                       "--select-buffers", "on"}).select_buffers);
    // A small macro has nothing to buffer, and the two-port macro no such strips.
    EXPECT_FALSE(parse({"--openroad", "--bitcell", "6t"}).select_buffers);
    EXPECT_FALSE(parse({"--openroad", "--num-wls", "128"}).select_buffers);
}

TEST(MainConfig, PrechargeBuffersComeOnWhenOneOutputCannotDriveEveryColumn) {
    // 128 kbit, 256 wordlines x 64 bits at 8:1: 64 IO blocks of 48 fins on blprechn, 323 fF.
    const std::vector<std::string> big = {"--openroad", "--bitcell", "6t", "--num-wls", "128", "--num-data-bits", "64",
                                          "--num-rows-per-mux", "8", "--segment-bits", "4"};
    EXPECT_TRUE(parse(big).precharge_buffers);
    EXPECT_TRUE(parse(big).select_buffers);
    auto off = big;
    off.insert(off.end(), {"--precharge-buffers", "off"});
    EXPECT_FALSE(parse(off).precharge_buffers);
    EXPECT_TRUE(parse(off).select_buffers);  // sel_lo is past the limit on its own
    // They stand in the select buffers' row: without it the line stays the controller's.
    auto no_row = big;
    no_row.insert(no_row.end(), {"--select-buffers", "off"});
    EXPECT_FALSE(parse(no_row).precharge_buffers);
    EXPECT_FALSE(parse(no_row).select_buffers);
    // 68 wordlines: the second group's eight slices leave the row no room for one.
    EXPECT_FALSE(parse({"--openroad", "--bitcell", "6t", "--num-wls", "34", "--num-data-bits", "64",
                        "--num-rows-per-mux", "8", "--segment-bits", "4"}).precharge_buffers);
    // Asked for on a small macro, they bring the select buffers with them.
    const MainCliOptions small = parse({"--openroad", "--bitcell", "6t", "--num-wls", "32", "--num-data-bits", "8",
                                        "--segment-bits", "2", "--precharge-buffers", "on"});
    EXPECT_TRUE(small.precharge_buffers);
    EXPECT_TRUE(small.select_buffers);
    // 16 kbit at 4:1 has nothing to buffer, and the two-port macro no such strips.
    EXPECT_FALSE(parse({"--openroad", "--bitcell", "6t", "--num-wls", "128", "--num-data-bits", "16"}).precharge_buffers);
    EXPECT_FALSE(parse({"--openroad", "--num-wls", "128"}).precharge_buffers);
}

TEST(MainConfig, SixTKeepsItsExistingOptInAndLegacyFlowStaysSeparate) {
    EXPECT_FALSE(parse({"--openroad", "--bitcell", "6t"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--bitcell", "6t", "--strips-in-controller"}).use_controller_band());
    EXPECT_FALSE(parse({"--single-port"}).use_controller_band());
    EXPECT_FALSE(parse({}).use_controller_band());
}
