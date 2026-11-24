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

qqr::SimulationConfig MakeSmallConfig()
{
    qqr::SimulationConfig config{};
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
    qqr::SimulationReport report{};
    ASSERT_EQ(qqr::RunSimulation("note.txt", file, MakeSmallConfig(), report, {}), qqr::SimulationCode::Ok);
    ASSERT_EQ(report.result, qqr::ReceiveCode::Ok);
    EXPECT_EQ(report.fileName, "note.txt");
    EXPECT_EQ(report.recoveredFile, file);
}

TEST(SimulationTest, MultiBlockFileSurvivesDroppedFrames)
{
    const std::vector<std::uint8_t> file = MakeFile(150000, 9);
    qqr::SimulationConfig config = MakeSmallConfig();
    config.dropRate = 0.4;
    qqr::SimulationReport report{};
    ASSERT_EQ(qqr::RunSimulation("archive.tar.gz", file, config, report, {}), qqr::SimulationCode::Ok);
    ASSERT_EQ(report.result, qqr::ReceiveCode::Ok);
    EXPECT_EQ(report.recoveredFile, file);
}

TEST(SimulationTest, TooSmallScreenIsBadLayout)
{
    qqr::SimulationConfig config = MakeSmallConfig();
    config.screenWidth = 200;
    qqr::SimulationReport report{};
    EXPECT_EQ(qqr::RunSimulation("x", std::vector<std::uint8_t>{1}, config, report, {}), qqr::SimulationCode::BadLayout);
}
