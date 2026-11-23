#include <gtest/gtest.h>

#include "CameraSimulator.hpp"
#include "FrameLayout.hpp"
#include "FrameReader.hpp"
#include "FrameRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

vfdt::Image RotateClockwise(const vfdt::Image& image)
{
    vfdt::Image rotated = vfdt::MakeImage(image.height, image.width);
    for (int y = 0; y < image.height; ++y)
    {
        for (int x = 0; x < image.width; ++x)
        {
            const std::size_t from = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 3;
            const int targetX = image.height - 1 - y;
            const int targetY = x;
            const std::size_t to = (static_cast<std::size_t>(targetY) * static_cast<std::size_t>(rotated.width) + static_cast<std::size_t>(targetX)) * 3;
            std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(from), 3, rotated.pixels.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    return rotated;
}

// Converts as a video encoder does: video-range BT.709 with each chroma sample averaged over a 2x2 block.
vfdt::Image ToYuv420(const vfdt::Image& rgb)
{
    vfdt::Image yuv = vfdt::MakeYuv420Image(rgb.width, rgb.height);
    const std::size_t width = static_cast<std::size_t>(rgb.width);
    const std::size_t chromaWidth = static_cast<std::size_t>(vfdt::ChromaSize(rgb.width));
    const std::size_t lumaBytes = width * static_cast<std::size_t>(rgb.height);
    const std::size_t chromaBytes = (yuv.pixels.size() - lumaBytes) / 2;
    std::vector<double> sumU(chromaBytes, 0.0);
    std::vector<double> sumV(chromaBytes, 0.0);
    std::vector<int> counts(chromaBytes, 0);
    const auto toByte = [](double value)
    {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    for (std::size_t y = 0; y < static_cast<std::size_t>(rgb.height); ++y)
    {
        for (std::size_t x = 0; x < width; ++x)
        {
            const std::uint8_t* pixel = rgb.pixels.data() + (y * width + x) * 3;
            const double luma = 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
            yuv.pixels[y * width + x] = toByte(16.0 + luma * 219.0 / 255.0);
            const std::size_t chroma = (y / 2) * chromaWidth + x / 2;
            sumU[chroma] += (pixel[2] - luma) / 1.8556;
            sumV[chroma] += (pixel[0] - luma) / 1.5748;
            ++counts[chroma];
        }
    }
    for (std::size_t i = 0; i < chromaBytes; ++i)
    {
        yuv.pixels[lumaBytes + i] = toByte(128.0 + sumU[i] / counts[i] * 224.0 / 255.0);
        yuv.pixels[lumaBytes + chromaBytes + i] = toByte(128.0 + sumV[i] / counts[i] * 224.0 / 255.0);
    }
    return yuv;
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

TEST(FrameReaderTest, ReadsVideoFrameInYuv420)
{
    const vfdt::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 9);
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const vfdt::Image screen = renderer.Render(payload, 1);
    vfdt::CameraSimulator camera{MakeCamera(), screen.width, screen.height};
    const vfdt::Image capture = ToYuv420(camera.Capture(screen, screen, 1.0));
    ASSERT_EQ(capture.format, vfdt::PixelFormat::kYuv420);
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(capture, read), vfdt::ReadCode::Ok);
    EXPECT_EQ(read.gridWidth, grid_width);
    EXPECT_EQ(read.gridHeight, grid_height);
    EXPECT_EQ(read.parity, 1U);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsTwoColourVideoFrameInYuv420)
{
    const vfdt::FrameLayout layout = MakeLayout(1);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 10);
    const vfdt::Image screen = vfdt::FrameRenderer{layout, cell_pixels}.Render(payload, 0);
    const vfdt::FrameReader reader{1};
    vfdt::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(ToYuv420(screen), read), vfdt::ReadCode::Ok);
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

TEST(FrameReaderTest, ReadsRecordingRotatedEachWay)
{
    const vfdt::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 8);
    vfdt::Image image = vfdt::FrameRenderer{layout, cell_pixels}.Render(payload, 0);
    const vfdt::FrameReader reader{2};
    for (int turn = 1; turn <= 3; ++turn)
    {
        image = RotateClockwise(image);
        vfdt::FrameRead read{};
        ASSERT_EQ(reader.ReadFrame(image, read), vfdt::ReadCode::Ok) << "after " << turn << " quarter turns";
        EXPECT_EQ(read.gridWidth, grid_width);
        EXPECT_EQ(read.gridHeight, grid_height);
        EXPECT_EQ(read.payload, payload);
    }
}

TEST(FrameReaderTest, BlankImageHasNoFinders)
{
    vfdt::Image blank = vfdt::MakeImage(300, 200);
    const vfdt::FrameReader reader{2};
    vfdt::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(blank, read), vfdt::ReadCode::FindersNotFound);
}
