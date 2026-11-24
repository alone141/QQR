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
qqr::CalibrationPlan SmallPlan(std::vector<int> cells, int ticksPerStep)
{
    qqr::CalibrationPlan plan{};
    plan.screenWidth = 800;
    plan.screenHeight = 640;
    plan.cellSizes = std::move(cells);
    plan.colorBits = {2};
    plan.frameRates = {20.0};
    plan.refreshRate = 20.0;
    plan.secondsPerStep = ticksPerStep / 20.0;
    return plan;
}

std::vector<qqr::Image> RenderEveryTick(const qqr::CalibrationRenderer& renderer)
{
    std::vector<qqr::Image> images;
    for (std::uint64_t tick = 0; tick < renderer.TicksPerCycle(); ++tick)
    {
        images.push_back(renderer.Render(tick));
    }
    return images;
}

} // namespace

TEST(CalibrationTest, HeaderRoundTripsInEveryCodeword)
{
    qqr::CalibrationHeader header{};
    header.frameTick = 0x12345678U;
    header.ticksPerStep = 60;
    header.tickRateTenths = 600;
    header.settings = {{10, 2, 2}, {6, 1, 3}, {4, 2, 1}};
    const std::vector<std::uint8_t> payload = qqr::MakeCalibrationPayload(header, 3 * qqr::rs_data_bytes);
    const std::optional<qqr::CalibrationHeader> parsed =
        qqr::ParseCalibrationHeader(std::span<const std::uint8_t>{payload}.subspan(2 * qqr::rs_data_bytes, qqr::rs_data_bytes));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->frameTick, header.frameTick);
    EXPECT_EQ(parsed->ticksPerStep, header.ticksPerStep);
    EXPECT_EQ(parsed->tickRateTenths, header.tickRateTenths);
    EXPECT_EQ(parsed->settings, header.settings);
}

TEST(CalibrationTest, OtherDataHasNoHeader)
{
    const std::vector<std::uint8_t> zeros(qqr::rs_data_bytes, 0);
    EXPECT_FALSE(qqr::ParseCalibrationHeader(zeros).has_value());
}

TEST(CalibrationTest, RendererRejectsBadPlans)
{
    qqr::CalibrationRenderer renderer{};
    EXPECT_EQ(renderer.Configure(SmallPlan({}, 10)), qqr::CalibrationCode::BadStepCount);
    EXPECT_EQ(renderer.Configure(SmallPlan({20}, 10)), qqr::CalibrationCode::BadCellSize);
    EXPECT_EQ(renderer.Configure(SmallPlan({6}, 0)), qqr::CalibrationCode::BadTiming);

    qqr::CalibrationPlan tooMany = SmallPlan({8, 8, 8, 8, 8, 8, 8, 8, 8}, 10);
    tooMany.colorBits = {2, 1};
    tooMany.frameRates = {20.0, 10.0};
    EXPECT_EQ(renderer.Configure(tooMany), qqr::CalibrationCode::BadStepCount);

    qqr::CalibrationPlan badColors = SmallPlan({6}, 10);
    badColors.colorBits = {2, 3};
    EXPECT_EQ(renderer.Configure(badColors), qqr::CalibrationCode::BadBitsPerCell);

    // At 2 frames/s on a 20 Hz screen each frame takes 10 ticks; a 5-tick step cannot show one.
    qqr::CalibrationPlan tooSlow = SmallPlan({6}, 5);
    tooSlow.frameRates = {20.0, 2.0};
    EXPECT_EQ(renderer.Configure(tooSlow), qqr::CalibrationCode::BadTiming);
}

TEST(CalibrationTest, StepsCoverEveryRateColourAndCellSize)
{
    qqr::CalibrationPlan plan = SmallPlan({8, 6}, 6);
    plan.colorBits = {2, 1};
    plan.frameRates = {30.0, 20.0, 29.0};
    plan.refreshRate = 60.0;
    plan.secondsPerStep = 0.1;
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    // 30 and 20 frames/s are 2 and 3 refreshes, so ticks run at the full 60 Hz; 29 rounds to 30 and is dropped.
    EXPECT_DOUBLE_EQ(renderer.TickRate(), 60.0);
    const std::vector<qqr::CalibrationSetting> expected{{8, 2, 2}, {6, 2, 2}, {8, 1, 2}, {6, 1, 2},
                                                         {8, 2, 3}, {6, 2, 3}, {8, 1, 3}, {6, 1, 3}};
    EXPECT_EQ(renderer.Settings(), expected);
    EXPECT_EQ(renderer.TicksPerCycle(), 48U);
    EXPECT_EQ(renderer.StepAt(5), 0U);
    EXPECT_EQ(renderer.StepAt(6), 1U);
    EXPECT_EQ(renderer.StepAt(48), 0U);
    const qqr::Image image = renderer.Render(47);
    EXPECT_EQ(image.width, 800);
    EXPECT_EQ(image.height, 640);
}

TEST(CalibrationTest, HeldFramesRepeatTheSameImage)
{
    qqr::CalibrationPlan plan = SmallPlan({6}, 6);
    plan.frameRates = {10.0};
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    EXPECT_DOUBLE_EQ(renderer.TickRate(), 10.0);

    plan.frameRates = {20.0, 10.0};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    EXPECT_DOUBLE_EQ(renderer.TickRate(), 20.0);
    // The second step holds each frame for 2 ticks: ticks 6 and 7 are one frame, 8 the next.
    EXPECT_EQ(renderer.FrameStartAt(7), 6U);
    EXPECT_EQ(renderer.Render(6).pixels, renderer.Render(7).pixels);
    EXPECT_NE(renderer.Render(7).pixels, renderer.Render(8).pixels);
    EXPECT_NE(renderer.Render(0).pixels, renderer.Render(1).pixels);
}

TEST(CalibrationTest, CleanFramesRecommendFastestCombination)
{
    qqr::CalibrationPlan plan = SmallPlan({8, 6, 4}, 2);
    plan.colorBits = {1, 2};
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    qqr::CalibrationAnalyzer analyzer{2};
    analyzer.AcceptImages(RenderEveryTick(renderer));
    const qqr::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 6U);
    for (const qqr::CalibrationStep& step : report.steps)
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
    qqr::CalibrationPlan plan = SmallPlan({6}, 4);
    plan.frameRates = {20.0, 10.0};
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    qqr::CalibrationAnalyzer analyzer{2};
    analyzer.AcceptImages(RenderEveryTick(renderer));
    const qqr::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 2U);
    const qqr::CalibrationStep& fast = report.steps[0];
    const qqr::CalibrationStep& slow = report.steps[1];
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
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(SmallPlan({6}, 3)), qqr::CalibrationCode::Ok);
    std::vector<qqr::Image> images;
    for (std::uint64_t tick = 0; tick < renderer.TicksPerCycle(); ++tick)
    {
        // First a capture torn mid-switch: a band in the middle already shows the next frame. Then a clean one.
        qqr::Image torn = renderer.Render(tick);
        const qqr::Image next = renderer.Render(tick + 1);
        const std::size_t rowBytes = static_cast<std::size_t>(torn.width) * 3;
        const std::size_t begin = static_cast<std::size_t>(torn.height) * 48 / 100 * rowBytes;
        const std::size_t end = static_cast<std::size_t>(torn.height) * 52 / 100 * rowBytes;
        std::copy(next.pixels.begin() + static_cast<std::ptrdiff_t>(begin), next.pixels.begin() + static_cast<std::ptrdiff_t>(end),
                  torn.pixels.begin() + static_cast<std::ptrdiff_t>(begin));
        images.push_back(std::move(torn));
        images.push_back(renderer.Render(tick));
    }
    qqr::CalibrationAnalyzer analyzer{2};
    analyzer.AcceptImages(images);
    const qqr::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    const qqr::CalibrationStep& step = report.steps[0];
    EXPECT_EQ(step.framesRead, 6U) << "blended/torn " << step.blendedOrTorn << ", lost " << step.notLocated;
    EXPECT_EQ(step.maxCorrectedBytes, 0);
    EXPECT_DOUBLE_EQ(step.frameYield, 1.0);
    EXPECT_TRUE(step.reliable);
}

TEST(CalibrationTest, ImagesWithoutCalibrationFramesFindNothing)
{
    qqr::CalibrationAnalyzer analyzer{1};
    const std::vector<qqr::Image> images{qqr::MakeImage(320, 240)};
    analyzer.AcceptImages(images);
    const qqr::CalibrationReport report = analyzer.Report();
    EXPECT_FALSE(report.found);
    EXPECT_EQ(report.unattributed, 1U);
    EXPECT_EQ(report.recommendedStep, -1);
}

TEST(CalibrationTest, SimulatedCameraRejectsUnreadableCells)
{
    qqr::CalibrationPlan plan = SmallPlan({8, 2, 8}, 10);
    plan.screenWidth = 1280;
    plan.screenHeight = 720;
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    qqr::CameraModel camera{};
    camera.width = 1920;
    camera.height = 1080;
    qqr::CalibrationAnalyzer analyzer{4};
    qqr::SimulateCalibration(renderer, camera, 60.0, renderer.TicksPerCycle(), analyzer);
    const qqr::CalibrationReport report = analyzer.Report();
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
    qqr::CalibrationPlan plan = SmallPlan({10, 8}, 12);
    plan.screenWidth = 1280;
    plan.screenHeight = 720;
    plan.colorBits = {2, 1};
    plan.frameRates = {30.0, 20.0};
    plan.refreshRate = 60.0;
    plan.secondsPerStep = 0.5;
    qqr::CalibrationRenderer renderer{};
    ASSERT_EQ(renderer.Configure(plan), qqr::CalibrationCode::Ok);
    qqr::CameraModel camera{};
    camera.width = 1920;
    camera.height = 1080;
    qqr::CalibrationAnalyzer analyzer{4};
    qqr::SimulateCalibration(renderer, camera, 60.0, renderer.TicksPerCycle(), analyzer);
    const qqr::CalibrationReport report = analyzer.Report();
    ASSERT_TRUE(report.found);
    ASSERT_EQ(report.steps.size(), 8U);
    for (const qqr::CalibrationStep& step : report.steps)
    {
        EXPECT_GT(step.framesRead, 0U) << step.cellPixels << " px, " << step.bitsPerCell << " bits, " << step.framesPerSecond << " fps";
        EXPECT_EQ(step.wrongCodewords, 0U);
    }
    ASSERT_GE(report.recommendedStep, 0);
}
