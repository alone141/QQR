#include <gtest/gtest.h>

#include "Simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace
{

std::vector<std::uint8_t> MakeFile(std::size_t size, std::uint32_t seed)
{
    std::mt19937 random{seed};
    std::vector<std::uint8_t> file(size);
    for (std::uint8_t& byte : file)
    {
        byte = static_cast<std::uint8_t>(random());
    }
    return file;
}

vfdt::SimulationConfig MakeSmallConfig()
{
    vfdt::SimulationConfig config{};
    config.screenWidth = 640;
    config.screenHeight = 360;
    config.cellPixels = 4;
    config.camera.width = 1000;
    config.camera.height = 600;
    config.camera.blurSigma = 0.7;
    config.camera.noiseSigma = 2.5;
    return config;
}

} // namespace

TEST(SimulationTest, SmallTextFileArrivesIntact)
{
    const std::vector<std::uint8_t> file(2048, 'a');
    vfdt::SimulationReport report{};
    ASSERT_EQ(vfdt::RunSimulation("note.txt", file, MakeSmallConfig(), report, {}), vfdt::SimulationCode::Ok);
    ASSERT_EQ(report.result, vfdt::ReceiveCode::Ok);
    EXPECT_EQ(report.fileName, "note.txt");
    EXPECT_EQ(report.recoveredFile, file);
}

TEST(SimulationTest, MultiBlockFileSurvivesDroppedFrames)
{
    const std::vector<std::uint8_t> file = MakeFile(150000, 9);
    vfdt::SimulationConfig config = MakeSmallConfig();
    config.dropRate = 0.4;
    vfdt::SimulationReport report{};
    ASSERT_EQ(vfdt::RunSimulation("archive.tar.gz", file, config, report, {}), vfdt::SimulationCode::Ok);
    ASSERT_EQ(report.result, vfdt::ReceiveCode::Ok);
    EXPECT_EQ(report.recoveredFile, file);
}

TEST(SimulationTest, TooSmallScreenIsBadLayout)
{
    vfdt::SimulationConfig config = MakeSmallConfig();
    config.screenWidth = 200;
    vfdt::SimulationReport report{};
    EXPECT_EQ(vfdt::RunSimulation("x", std::vector<std::uint8_t>{1}, config, report, {}), vfdt::SimulationCode::BadLayout);
}
