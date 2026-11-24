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

qqr::FrameLayout MakeLayout(int bitsPerCell)
{
    qqr::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(grid_width, grid_height, bitsPerCell), qqr::LayoutCode::Ok);
    return layout;
}

std::vector<std::uint8_t> MakePayload(const qqr::FrameLayout& layout, std::uint32_t seed)
{
    std::mt19937 random{seed};
    std::vector<std::uint8_t> payload(layout.PayloadBytes());
    for (std::uint8_t& byte : payload)
    {
        byte = static_cast<std::uint8_t>(random());
    }
    return payload;
}

qqr::Image RotateClockwise(const qqr::Image& image)
{
    qqr::Image rotated = qqr::MakeImage(image.height, image.width);
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
qqr::Image ToYuv420(const qqr::Image& rgb)
{
    qqr::Image yuv = qqr::MakeYuv420Image(rgb.width, rgb.height);
    const std::size_t width = static_cast<std::size_t>(rgb.width);
    const std::size_t chromaWidth = static_cast<std::size_t>(qqr::ChromaSize(rgb.width));
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

qqr::CameraModel MakeCamera()
{
    qqr::CameraModel camera{};
    camera.width = 800;
    camera.height = 560;
    camera.blurSigma = 0.7;
    camera.noiseSigma = 2.0;
    return camera;
}

} // namespace

TEST(FrameReaderTest, ReadsScreenImageExactly)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 1);
    const qqr::Image image = qqr::FrameRenderer{layout, cell_pixels}.Render(payload, 4);
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(image, read), qqr::ReadCode::Ok);
    EXPECT_EQ(read.gridWidth, grid_width);
    EXPECT_EQ(read.gridHeight, grid_height);
    EXPECT_EQ(read.parity, 0U);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.correctedBytes, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsTwoColourFrame)
{
    const qqr::FrameLayout layout = MakeLayout(1);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 2);
    const qqr::Image image = qqr::FrameRenderer{layout, cell_pixels}.Render(payload, 1);
    const qqr::FrameReader reader{1};
    qqr::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(image, read), qqr::ReadCode::Ok);
    EXPECT_EQ(read.parity, 1U);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsTiltedNoisyCameraCapture)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 3);
    const qqr::FrameRenderer renderer{layout, cell_pixels};
    const qqr::Image screen = renderer.Render(payload, 0);
    qqr::CameraSimulator camera{MakeCamera(), screen.width, screen.height};
    const qqr::Image capture = camera.Capture(screen, screen, 1.0);
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(capture, read), qqr::ReadCode::Ok);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsFrameWithPassCounterInQuietZone)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 9);
    const qqr::FrameRenderer renderer{layout, cell_pixels};
    qqr::Image screen = renderer.Render(payload, 0);

    // A solid block where the player draws "pass N.NN": worse than the real text.
    const int border = qqr::quiet_zone_cells * cell_pixels;
    const int labelHeight = border - 2;
    const int labelWidth = 9 * labelHeight;
    for (int y = screen.height - border + 1; y < screen.height - 1; ++y)
    {
        for (int x = (screen.width - labelWidth) / 2; x < (screen.width + labelWidth) / 2; ++x)
        {
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(screen.width) + static_cast<std::size_t>(x)) * 3;
            std::fill_n(screen.pixels.begin() + static_cast<std::ptrdiff_t>(at), 3, std::uint8_t{96});
        }
    }

    qqr::CameraSimulator camera{MakeCamera(), screen.width, screen.height};
    const qqr::Image capture = camera.Capture(screen, screen, 1.0);
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(capture, read), qqr::ReadCode::Ok);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsVideoFrameInYuv420)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 9);
    const qqr::FrameRenderer renderer{layout, cell_pixels};
    const qqr::Image screen = renderer.Render(payload, 1);
    qqr::CameraSimulator camera{MakeCamera(), screen.width, screen.height};
    const qqr::Image capture = ToYuv420(camera.Capture(screen, screen, 1.0));
    ASSERT_EQ(capture.format, qqr::PixelFormat::kYuv420);
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(capture, read), qqr::ReadCode::Ok);
    EXPECT_EQ(read.gridWidth, grid_width);
    EXPECT_EQ(read.gridHeight, grid_height);
    EXPECT_EQ(read.parity, 1U);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, ReadsTwoColourVideoFrameInYuv420)
{
    const qqr::FrameLayout layout = MakeLayout(1);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 10);
    const qqr::Image screen = qqr::FrameRenderer{layout, cell_pixels}.Render(payload, 0);
    const qqr::FrameReader reader{1};
    qqr::FrameRead read{};
    ASSERT_EQ(reader.ReadFrame(ToYuv420(screen), read), qqr::ReadCode::Ok);
    EXPECT_EQ(read.failedCodewords, 0);
    EXPECT_EQ(read.payload, payload);
}

TEST(FrameReaderTest, MixedCaptureIsBlended)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const qqr::FrameRenderer renderer{layout, cell_pixels};
    const qqr::Image first = renderer.Render(MakePayload(layout, 4), 0);
    const qqr::Image second = renderer.Render(MakePayload(layout, 5), 1);
    qqr::CameraModel model = MakeCamera();
    model.readout = 0.0;
    model.exposure = 0.01;
    qqr::CameraSimulator camera{model, first.width, first.height};
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(camera.Capture(first, second, 0.005), read), qqr::ReadCode::BlendedFrame);
}

TEST(FrameReaderTest, RollingShutterTearIsTorn)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const qqr::FrameRenderer renderer{layout, cell_pixels};
    const qqr::Image first = renderer.Render(MakePayload(layout, 6), 0);
    const qqr::Image second = renderer.Render(MakePayload(layout, 7), 1);
    qqr::CameraModel model = MakeCamera();
    model.readout = 0.02;
    model.exposure = 0.0005;
    qqr::CameraSimulator camera{model, first.width, first.height};
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(camera.Capture(first, second, 0.01), read), qqr::ReadCode::TornFrame);
}

TEST(FrameReaderTest, ReadsRecordingRotatedEachWay)
{
    const qqr::FrameLayout layout = MakeLayout(2);
    const std::vector<std::uint8_t> payload = MakePayload(layout, 8);
    qqr::Image image = qqr::FrameRenderer{layout, cell_pixels}.Render(payload, 0);
    const qqr::FrameReader reader{2};
    for (int turn = 1; turn <= 3; ++turn)
    {
        image = RotateClockwise(image);
        qqr::FrameRead read{};
        ASSERT_EQ(reader.ReadFrame(image, read), qqr::ReadCode::Ok) << "after " << turn << " quarter turns";
        EXPECT_EQ(read.gridWidth, grid_width);
        EXPECT_EQ(read.gridHeight, grid_height);
        EXPECT_EQ(read.payload, payload);
    }
}

TEST(FrameReaderTest, BlankImageHasNoFinders)
{
    qqr::Image blank = qqr::MakeImage(300, 200);
    const qqr::FrameReader reader{2};
    qqr::FrameRead read{};
    EXPECT_EQ(reader.ReadFrame(blank, read), qqr::ReadCode::FindersNotFound);
}
