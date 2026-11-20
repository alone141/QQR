#include <gtest/gtest.h>

#include "CameraSimulator.hpp"
#include "FrameLayout.hpp"
#include "FrameReader.hpp"
#include "FrameRenderer.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace
{

constexpr int cell_pixels = 4;
constexpr int grid_width = 120;
constexpr int grid_height = 80;

vfdt::FrameLayout MakeLayout(int bitsPerCell)
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(grid_width, grid_height, bitsPerCell), vfdt::LayoutCode::Ok);
    return layout;
}

std::vector<std::uint8_t> MakePayload(const vfdt::FrameLayout& layout, std::uint32_t seed)
{
    std::mt19937 random{seed};
    std::vector<std::uint8_t> payload(layout.PayloadBytes());
    for (std::uint8_t& byte : payload)
    {
        byte = static_cast<std::uint8_t>(random());
    }
    return payload;
}

vfdt::CameraModel MakeCamera()
{
    vfdt::CameraModel camera{};
    camera.width = 800;
    camera.height = 560;
    camera.blurSigma = 0.7;
    camera.noiseSigma = 2.0;
    return camera;
}

} // namespace

TEST(FrameReaderTest, ReadsScreenImageExactly)
{
    const vfdt::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 1);
    const vfdt::Image image = vfdt::FrameRenderer{layout, cell_pixels}.Render(payload, 4);
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(image, read), vfdt::ReadCode::Ok);
    EXPECT_EQ(read.gridWidth, grid_width);
    EXPECT_EQ(read.gridHeight, grid_height);
    EXPECT_EQ(read.parity, 0U);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.correctedBytes, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsTwoColourFrame)
{
    const vfdt::FrameLayout layout = MakeLayout(1);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 2);
    const vfdt::Image image = vfdt::FrameRenderer{layout, cell_pixels}.Render(payload, 1);
    const vfdt::FrameReader reader{1};
    vfdt::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(image, read), vfdt::ReadCode::Ok);
    EXPECT_EQ(read.parity, 1U);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsTiltedNoisyCameraCapture)
{
    const vfdt::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 3);
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const vfdt::Image screen = renderer.Render(payload, 0);
    vfdt::CameraSimulator camera{MakeCamera(), screen.width, screen.height};
    const vfdt::Image capture = camera.Capture(screen, screen, 1.0);
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(capture, read), vfdt::ReadCode::Ok);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, MixedCaptureIsBlended)
{
    const vfdt::FrameLayout layout = MakeLayout(2);
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const vfdt::Image first = renderer.Render(MakePayload(layout, 4), 0);
    const vfdt::Image second = renderer.Render(MakePayload(layout, 5), 1);
    vfdt::CameraModel model = MakeCamera();
    model.readout = 0.0;
    model.exposure = 0.01;
    vfdt::CameraSimulator camera{model, first.width, first.height};
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(camera.Capture(first, second, 0.005), read), vfdt::ReadCode::BlendedFrame);
}

TEST(FrameReaderTest, RollingShutterTearIsTorn)
{
    const vfdt::FrameLayout layout = MakeLayout(2);
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const vfdt::Image first = renderer.Render(MakePayload(layout, 6), 0);
    const vfdt::Image second = renderer.Render(MakePayload(layout, 7), 1);
    vfdt::CameraModel model = MakeCamera();
    model.readout = 0.02;
    model.exposure = 0.0005;
    vfdt::CameraSimulator camera{model, first.width, first.height};
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(camera.Capture(first, second, 0.01), read), vfdt::ReadCode::TornFrame);
}

TEST(FrameReaderTest, BlankImageHasNoFinders)
{
    vfdt::Image blank = vfdt::MakeImage(300, 200);
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(blank, read), vfdt::ReadCode::FindersNotFound);
}
