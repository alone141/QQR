#include <gtest/gtest.h>

#include "Calibration.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace
{

vfdt::CalibrationPlan SmallPlan(std::vector<int> cells, int framesPerStep)
{
    vfdt::CalibrationPlan plan{};
    plan.screenWidth = 800;
    plan.screenHeight = 640;
    plan.cellSizes = std::move(cells);
    plan.framesPerStep = framesPerStep;
    return plan;
}

} // namespace

TEST(CalibrationTest, HeaderRoundTripsInEveryCodeword)
{
    vfdt::CalibrationHeader header{};
    header.frameIndex = 0x12345678U;
    header.framesPerStep = 60;
    header.fpsTenths = 200;
    header.bitsPerCell = 2;
    header.cellSizes = {10, 6, 4};
    const std::vector<std::uint8_t> payload = vfdt::MakeCalibrationPayload(header, 3 * vfdt::rs_data_bytes);
    const std::optional<vfdt::CalibrationHeader> parsed =
        vfdt::ParseCalibrationHeader(std::span<const std::uint8_t>{payload}.subspan(2 * vfdt::rs_data_bytes, vfdt::rs_data_bytes));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frameIndex, header.frameIndex);
    EXPECT_EQ(parsed->framesPerStep, header.framesPerStep);
    EXPECT_EQ(parsed->fpsTenths, header.fpsTenths);
    EXPECT_EQ(parsed->bitsPerCell, header.bitsPerCell);
    EXPECT_EQ(parsed->cellSizes, header.cellSizes);
}

TEST(CalibrationTest, OtherDataHasNoHeader)
{
    const std::vector<std::uint8_t> zeros(vfdt::rs_data_bytes, 0);
    EXPECT_FALSE(vfdt::ParseCalibrationHeader(zeros).has_value());
}

TEST(CalibrationTest, RendererRejectsBadPlans)
{
    vfdt::CalibrationRenderer renderer{};
    EXPECT_EQ(renderer.Configure(SmallPlan({}, 10)), vfdt::CalibrationCode::BadStepCount);
    EXPECT_EQ(renderer.Configure(SmallPlan({8, 8, 8, 8, 8, 8, 8, 8, 8}, 10)), vfdt::CalibrationCode::BadStepCount);
    EXPECT_EQ(renderer.Configure(SmallPlan({20}, 10)), vfdt::CalibrationCode::BadCellSize);
    EXPECT_EQ(renderer.Configure(SmallPlan({6}, 0)), vfdt::CalibrationCode::BadTiming);
}

TEST(CalibrationTest, FramesCycleThroughCellSizesAtScreenSize)
{
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(SmallPlan({8, 6, 4}, 2)), vfdt::CalibrationCode::Ok);
    EXPECT_EQ(renderer.FramesPerCycle(), 6U);
    EXPECT_EQ(renderer.CellPixelsAt(0), 8);
    EXPECT_EQ(renderer.CellPixelsAt(3), 6);
    EXPECT_EQ(renderer.CellPixelsAt(5), 4);
    EXPECT_EQ(renderer.CellPixelsAt(6), 8);
    const vfdt::Image image = renderer.Render(5);
    EXPECT_EQ(image.width, 800);
    EXPECT_EQ(image.height, 640);
}

TEST(CalibrationTest, CleanFramesRecommendSmallestCells)
{
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(SmallPlan({8, 6, 4}, 2)), vfdt::CalibrationCode::Ok);
    std::vector<vfdt::Image> images;
    for (std::uint64_t frame = 0; frame < renderer.FramesPerCycle(); ++frame)
    {
        images.push_back(renderer.Render(frame));
    }
    vfdt::CalibrationAnalyzer analyzer{2, 2};
    analyzer.AcceptImages(images);
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 3U);
    for (const vfdt::CalibrationStep& step : report.steps)
    {
        EXPECT_EQ(step.framesRead, 2U) << step.cellPixels;
        EXPECT_DOUBLE_EQ(step.frameYield, 1.0) << step.cellPixels;
        EXPECT_EQ(step.correctedBytes, 0U) << step.cellPixels;
        EXPECT_TRUE(step.reliable) << step.cellPixels;
    }
    EXPECT_EQ(report.recommendedStep, 2);
}

TEST(CalibrationTest, FramesAreJudgedByTheirBestCapture)
{
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(SmallPlan({6}, 3)), vfdt::CalibrationCode::Ok);
    std::vector<vfdt::Image> images;
    for (std::uint64_t frame = 0; frame < renderer.FramesPerCycle(); ++frame)
    {
        // First a capture torn mid-switch: a band in the middle already shows the next frame. Then a clean one.
        vfdt::Image torn = renderer.Render(frame);
        const vfdt::Image next = renderer.Render(frame + 1);
        const std::size_t rowBytes = static_cast<std::size_t>(torn.width) * 3;
        const std::size_t begin = static_cast<std::size_t>(torn.height) * 48 / 100 * rowBytes;
        const std::size_t end = static_cast<std::size_t>(torn.height) * 52 / 100 * rowBytes;
        std::copy(next.pixels.begin() + static_cast<std::ptrdiff_t>(begin), next.pixels.begin() + static_cast<std::ptrdiff_t>(end),
                  torn.pixels.begin() + static_cast<std::ptrdiff_t>(begin));
        images.push_back(std::move(torn));
        images.push_back(renderer.Render(frame));
    }
    vfdt::CalibrationAnalyzer analyzer{2, 2};
    analyzer.AcceptImages(images);
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    const vfdt::CalibrationStep& step = report.steps[0];
    EXPECT_EQ(step.framesRead, 6U) << "blended/torn " << step.blendedOrTorn << ", lost " << step.notLocated;
    EXPECT_EQ(step.maxCorrectedBytes, 0);
    EXPECT_DOUBLE_EQ(step.frameYield, 1.0);
    EXPECT_TRUE(step.reliable);
}

TEST(CalibrationTest, ImagesWithoutCalibrationFramesFindNothing)
{
    vfdt::CalibrationAnalyzer analyzer{2, 1};
    const std::vector<vfdt::Image> images{vfdt::MakeImage(320, 240)};
    analyzer.AcceptImages(images);
    const vfdt::CalibrationReport report = analyzer.Report();
    EXPECT_FALSE(report.found);
    EXPECT_EQ(report.unattributed, 1U);
    EXPECT_EQ(report.recommendedStep, -1);
}

TEST(CalibrationTest, SimulatedCameraRejectsUnreadableCells)
{
    vfdt::CalibrationPlan plan{};
    plan.screenWidth = 1280;
    plan.screenHeight = 720;
    plan.cellSizes = {8, 2, 8};
    plan.framesPerStep = 10;
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    vfdt::CameraModel camera{};
    camera.width = 1920;
    camera.height = 1080;
    vfdt::CalibrationAnalyzer analyzer{plan.bitsPerCell, 4};
    vfdt::SimulateCalibration(renderer, camera, 60.0, plan.displayFps, renderer.FramesPerCycle(), analyzer);
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    // The unreadable middle step still gets its captures, placed between the readable ones on either side.
    EXPECT_GT(report.steps[1].captures, 20U);
    EXPECT_EQ(report.steps[1].framesRead, 0U);
    EXPECT_EQ(report.steps[1].gridWidth, 0);
    EXPECT_FALSE(report.steps[1].reliable);
    EXPECT_TRUE(report.steps[0].reliable);
    EXPECT_NE(report.recommendedStep, 1);
    EXPECT_GE(report.recommendedStep, 0);
}
