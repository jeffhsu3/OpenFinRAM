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

TEST(MainConfig, SixTKeepsItsExistingOptInAndLegacyFlowStaysSeparate) {
    EXPECT_FALSE(parse({"--openroad", "--bitcell", "6t"}).use_controller_band());
    EXPECT_TRUE(parse({"--openroad", "--bitcell", "6t", "--strips-in-controller"}).use_controller_band());
    EXPECT_FALSE(parse({"--single-port"}).use_controller_band());
    EXPECT_FALSE(parse({}).use_controller_band());
}
