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

// One colour depth at 20 frames/s on a 20 Hz screen, so every tick is a new frame.
vfdt::CalibrationPlan SmallPlan(std::vector<int> cells, int ticksPerStep)
{
    vfdt::CalibrationPlan plan{};
    plan.screenWidth = 800;
    plan.screenHeight = 640;
    plan.cellSizes = std::move(cells);
    plan.colorBits = {2};
    plan.frameRates = {20.0};
    plan.refreshRate = 20.0;
    plan.secondsPerStep = ticksPerStep / 20.0;
    return plan;
}

std::vector<vfdt::Image> RenderEveryTick(const vfdt::CalibrationRenderer& renderer)
{
    std::vector<vfdt::Image> images;
    for (std::uint64_t tick = 0; tick < renderer.TicksPerCycle(); ++tick)
    {
        images.push_back(renderer.Render(tick));
    }
    return images;
}

} // namespace

TEST(CalibrationTest, HeaderRoundTripsInEveryCodeword)
{
    vfdt::CalibrationHeader header{};
    header.frameTick = 0x12345678U;
    header.ticksPerStep = 60;
    header.tickRateTenths = 600;
    header.settings = {{10, 2, 2}, {6, 1, 3}, {4, 2, 1}};
    const std::vector<std::uint8_t> payload = vfdt::MakeCalibrationPayload(header, 3 * vfdt::rs_data_bytes);
    const std::optional<vfdt::CalibrationHeader> parsed =
        vfdt::ParseCalibrationHeader(std::span<const std::uint8_t>{payload}.subspan(2 * vfdt::rs_data_bytes, vfdt::rs_data_bytes));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frameTick, header.frameTick);
    EXPECT_EQ(parsed->ticksPerStep, header.ticksPerStep);
    EXPECT_EQ(parsed->tickRateTenths, header.tickRateTenths);
    EXPECT_EQ(parsed->settings, header.settings);
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
    EXPECT_EQ(renderer.Configure(SmallPlan({20}, 10)), vfdt::CalibrationCode::BadCellSize);
    EXPECT_EQ(renderer.Configure(SmallPlan({6}, 0)), vfdt::CalibrationCode::BadTiming);

    vfdt::CalibrationPlan tooMany = SmallPlan({8, 8, 8, 8, 8, 8, 8, 8, 8}, 10);
    tooMany.colorBits = {2, 1};
    tooMany.frameRates = {20.0, 10.0};
    EXPECT_EQ(renderer.Configure(tooMany), vfdt::CalibrationCode::BadStepCount);

    vfdt::CalibrationPlan badColors = SmallPlan({6}, 10);
    badColors.colorBits = {2, 3};
    EXPECT_EQ(renderer.Configure(badColors), vfdt::CalibrationCode::BadBitsPerCell);

    // At 2 frames/s on a 20 Hz screen each frame takes 10 ticks; a 5-tick step cannot show one.
    vfdt::CalibrationPlan tooSlow = SmallPlan({6}, 5);
    tooSlow.frameRates = {20.0, 2.0};
    EXPECT_EQ(renderer.Configure(tooSlow), vfdt::CalibrationCode::BadTiming);
}

TEST(CalibrationTest, StepsCoverEveryRateColourAndCellSize)
{
    vfdt::CalibrationPlan plan = SmallPlan({8, 6}, 6);
    plan.colorBits = {2, 1};
    plan.frameRates = {30.0, 20.0, 29.0};
    plan.refreshRate = 60.0;
    plan.secondsPerStep = 0.1;
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    // 30 and 20 frames/s are 2 and 3 refreshes, so ticks run at the full 60 Hz; 29 rounds to 30 and is dropped.
    EXPECT_DOUBLE_EQ(renderer.TickRate(), 60.0);
    const std::vector<vfdt::CalibrationSetting> expected{{8, 2, 2}, {6, 2, 2}, {8, 1, 2}, {6, 1, 2},
                                                         {8, 2, 3}, {6, 2, 3}, {8, 1, 3}, {6, 1, 3}};
    EXPECT_EQ(renderer.Settings(), expected);
    EXPECT_EQ(renderer.TicksPerCycle(), 48U);
    EXPECT_EQ(renderer.StepAt(5), 0U);
    EXPECT_EQ(renderer.StepAt(6), 1U);
    EXPECT_EQ(renderer.StepAt(48), 0U);
    const vfdt::Image image = renderer.Render(47);
    EXPECT_EQ(image.width, 800);
    EXPECT_EQ(image.height, 640);
}

TEST(CalibrationTest, HeldFramesRepeatTheSameImage)
{
    vfdt::CalibrationPlan plan = SmallPlan({6}, 6);
    plan.frameRates = {10.0};
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    EXPECT_DOUBLE_EQ(renderer.TickRate(), 10.0);

    plan.frameRates = {20.0, 10.0};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    EXPECT_DOUBLE_EQ(renderer.TickRate(), 20.0);
    // The second step holds each frame for 2 ticks: ticks 6 and 7 are one frame, 8 the next.
    EXPECT_EQ(renderer.FrameStartAt(7), 6U);
    EXPECT_EQ(renderer.Render(6).pixels, renderer.Render(7).pixels);
    EXPECT_NE(renderer.Render(7).pixels, renderer.Render(8).pixels);
    EXPECT_NE(renderer.Render(0).pixels, renderer.Render(1).pixels);
}

TEST(CalibrationTest, CleanFramesRecommendFastestCombination)
{
    vfdt::CalibrationPlan plan = SmallPlan({8, 6, 4}, 2);
    plan.colorBits = {1, 2};
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    vfdt::CalibrationAnalyzer analyzer{2};
    analyzer.AcceptImages(RenderEveryTick(renderer));
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 6U);
    for (const vfdt::CalibrationStep& step : report.steps)
    {
        EXPECT_EQ(step.framesRead, 2U) << step.cellPixels << " px, " << step.bitsPerCell << " bits";
        EXPECT_DOUBLE_EQ(step.frameYield, 1.0) << step.cellPixels << " px, " << step.bitsPerCell << " bits";
        EXPECT_EQ(step.correctedBytes, 0U) << step.cellPixels << " px, " << step.bitsPerCell << " bits";
        EXPECT_TRUE(step.reliable) << step.cellPixels << " px, " << step.bitsPerCell << " bits";
    }
    EXPECT_EQ(report.steps[0].bitsPerCell, 1);
    EXPECT_EQ(report.steps[5].bitsPerCell, 2);
    EXPECT_EQ(report.recommendedStep, 5);
}

TEST(CalibrationTest, SlowerRatesCountEachHeldFrameOnce)
{
    vfdt::CalibrationPlan plan = SmallPlan({6}, 4);
    plan.frameRates = {20.0, 10.0};
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    vfdt::CalibrationAnalyzer analyzer{2};
    analyzer.AcceptImages(RenderEveryTick(renderer));
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 2U);
    const vfdt::CalibrationStep& fast = report.steps[0];
    const vfdt::CalibrationStep& slow = report.steps[1];
    EXPECT_DOUBLE_EQ(fast.framesPerSecond, 20.0);
    EXPECT_DOUBLE_EQ(slow.framesPerSecond, 10.0);
    EXPECT_EQ(fast.framesExpected, 4U);
    // Four captures of two frames.
    EXPECT_EQ(slow.framesRead, 4U);
    EXPECT_EQ(slow.framesExpected, 2U);
    EXPECT_DOUBLE_EQ(slow.frameYield, 1.0);
    EXPECT_DOUBLE_EQ(slow.bytesPerSecond * 2.0, fast.bytesPerSecond);
    EXPECT_EQ(report.recommendedStep, 0);
}

TEST(CalibrationTest, FramesAreJudgedByTheirBestCapture)
{
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(SmallPlan({6}, 3)), vfdt::CalibrationCode::Ok);
    std::vector<vfdt::Image> images;
    for (std::uint64_t tick = 0; tick < renderer.TicksPerCycle(); ++tick)
    {
        // First a capture torn mid-switch: a band in the middle already shows the next frame. Then a clean one.
        vfdt::Image torn = renderer.Render(tick);
        const vfdt::Image next = renderer.Render(tick + 1);
        const std::size_t rowBytes = static_cast<std::size_t>(torn.width) * 3;
        const std::size_t begin = static_cast<std::size_t>(torn.height) * 48 / 100 * rowBytes;
        const std::size_t end = static_cast<std::size_t>(torn.height) * 52 / 100 * rowBytes;
        std::copy(next.pixels.begin() + static_cast<std::ptrdiff_t>(begin), next.pixels.begin() + static_cast<std::ptrdiff_t>(end),
                  torn.pixels.begin() + static_cast<std::ptrdiff_t>(begin));
        images.push_back(std::move(torn));
        images.push_back(renderer.Render(tick));
    }
    vfdt::CalibrationAnalyzer analyzer{2};
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
    vfdt::CalibrationAnalyzer analyzer{1};
    const std::vector<vfdt::Image> images{vfdt::MakeImage(320, 240)};
    analyzer.AcceptImages(images);
    const vfdt::CalibrationReport report = analyzer.Report();
    EXPECT_FALSE(report.found);
    EXPECT_EQ(report.unattributed, 1U);
    EXPECT_EQ(report.recommendedStep, -1);
}

TEST(CalibrationTest, SimulatedCameraRejectsUnreadableCells)
{
    vfdt::CalibrationPlan plan = SmallPlan({8, 2, 8}, 10);
    plan.screenWidth = 1280;
    plan.screenHeight = 720;
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    vfdt::CameraModel camera{};
    camera.width = 1920;
    camera.height = 1080;
    vfdt::CalibrationAnalyzer analyzer{4};
    vfdt::SimulateCalibration(renderer, camera, 60.0, renderer.TicksPerCycle(), analyzer);
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    // The unreadable middle step still gets its captures, placed between the readable ones on either side.
    EXPECT_GT(report.steps[1].captures, 20U);
    // A stray codeword may still decode, but almost none of the data gets through.
    EXPECT_LT(report.steps[1].frameYield, 0.1);
    EXPECT_EQ(report.steps[1].wrongCodewords, 0U);
    EXPECT_FALSE(report.steps[1].reliable);
    EXPECT_TRUE(report.steps[0].reliable);
    EXPECT_NE(report.recommendedStep, 1);
    EXPECT_GE(report.recommendedStep, 0);
}

TEST(CalibrationTest, SimulatedCameraReadsMixedColoursAndRates)
{
    vfdt::CalibrationPlan plan = SmallPlan({10, 8}, 12);
    plan.screenWidth = 1280;
    plan.screenHeight = 720;
    plan.colorBits = {2, 1};
    plan.frameRates = {30.0, 20.0};
    plan.refreshRate = 60.0;
    plan.secondsPerStep = 0.5;
    vfdt::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), vfdt::CalibrationCode::Ok);
    vfdt::CameraModel camera{};
    camera.width = 1920;
    camera.height = 1080;
    vfdt::CalibrationAnalyzer analyzer{4};
    vfdt::SimulateCalibration(renderer, camera, 60.0, renderer.TicksPerCycle(), analyzer);
    const vfdt::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 8U);
    for (const vfdt::CalibrationStep& step : report.steps)
    {
        EXPECT_GT(step.framesRead, 0U) << step.cellPixels << " px, " << step.bitsPerCell << " bits, " << step.framesPerSecond << " fps";
        EXPECT_EQ(step.wrongCodewords, 0U);
    }
    ASSERT_GE(report.recommendedStep, 0);
}
