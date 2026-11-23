#include <gtest/gtest.h>

#include "Image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{

std::vector<std::uint8_t> WriteAndRead(const vfdt::Image& image)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vfdt_image_test.png";
    EXPECT_EQ(vfdt::WritePng(path, image), vfdt::ImageIoCode::Ok);
    std::ifstream file{path, std::ios::binary};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
}

void SetPixel(vfdt::Image& image, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 3;
    image.pixels[index] = r;
    image.pixels[index + 1] = g;
    image.pixels[index + 2] = b;
}

vfdt::Image MakeUniformYuv(int width, int height, std::uint8_t luma, std::uint8_t u, std::uint8_t v)
{
    vfdt::Image image = vfdt::MakeYuv420Image(width, height);
    const std::size_t lumaBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t chromaBytes = (image.pixels.size() - lumaBytes) / 2;
    std::fill_n(image.pixels.begin(), lumaBytes, luma);
    std::fill_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(lumaBytes), chromaBytes, u);
    std::fill_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(lumaBytes + chromaBytes), chromaBytes, v);
    return image;
}

} // namespace

TEST(ImageTest, Yuv420ImageHoldsLumaThenTwoHalfSizePlanes)
{
    const vfdt::Image image = vfdt::MakeYuv420Image(5, 3);
    EXPECT_EQ(image.format, vfdt::PixelFormat::kYuv420);
    EXPECT_EQ(vfdt::ChromaSize(5), 3);
    EXPECT_EQ(vfdt::ChromaSize(4), 2);
    EXPECT_EQ(image.pixels.size(), 5U * 3U + 2U * (3U * 2U));
    EXPECT_EQ(vfdt::MakeImage(5, 3).format, vfdt::PixelFormat::kRgb24);
}

TEST(ImageTest, SampleColorConvertsVideoRangeYuvToRgb)
{
    const std::array<double, 3> white = vfdt::SampleColor(MakeUniformYuv(8, 6, 235, 128, 128), 4.0, 3.0);
    const std::array<double, 3> black = vfdt::SampleColor(MakeUniformYuv(8, 6, 16, 128, 128), 4.0, 3.0);
    const std::array<double, 3> magenta = vfdt::SampleColor(MakeUniformYuv(8, 6, 78, 214, 230), 4.0, 3.0);
    for (std::size_t c = 0; c < 3; ++c)
    {
        EXPECT_NEAR(white[c], 255.0, 0.5);
        EXPECT_NEAR(black[c], 0.0, 0.5);
    }
    EXPECT_NEAR(magenta[0], 255.0, 3.0);
    EXPECT_NEAR(magenta[1], 0.0, 3.0);
    EXPECT_NEAR(magenta[2], 255.0, 3.0);
}

TEST(ImageTest, SampleColorGivesEachPixelTheChromaOfItsBlock)
{
    // U rises in the right half of its plane and V stays neutral: only blue and green may change, and only on the right.
    vfdt::Image image = MakeUniformYuv(16, 8, 126, 128, 128);
    const std::size_t lumaBytes = 16U * 8U;
    for (std::size_t row = 0; row < 4; ++row)
    {
        std::fill_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(lumaBytes + row * 8 + 4), 4, static_cast<std::uint8_t>(228));
    }
    const std::array<double, 3> left = vfdt::SampleColor(image, 3.0, 4.0);
    const std::array<double, 3> right = vfdt::SampleColor(image, 13.0, 4.0);
    EXPECT_NEAR(left[2], left[0], 0.5);
    EXPECT_NEAR(right[0], left[0], 0.5);
    EXPECT_GT(right[2], left[2] + 150.0);
    EXPECT_LT(right[1], left[1] - 10.0);

    // Chroma column 4 covers pixel columns 8 and 9, so the step sits between the centres of pixels 7 and 8.
    EXPECT_NEAR(vfdt::SampleColor(image, 7.5, 4.0)[2], left[2], 0.5);
    EXPECT_NEAR(vfdt::SampleColor(image, 8.5, 4.0)[2], right[2], 0.5);
    EXPECT_NEAR(vfdt::SampleColor(image, 8.0, 4.0)[2], (left[2] + right[2]) / 2.0, 0.5);
}

TEST(ImageTest, SampleColorOnRgbMatchesSampleBilinear)
{
    vfdt::Image image = vfdt::MakeImage(4, 4);
    SetPixel(image, 1, 1, 200, 100, 50);
    SetPixel(image, 2, 1, 20, 220, 90);
    EXPECT_EQ(vfdt::SampleColor(image, 2.0, 1.7), vfdt::SampleBilinear(image, 2.0, 1.7));
}

TEST(ImageTest, FourColourImageIsTwoBitIndexedPng)
{
    vfdt::Image image = vfdt::MakeImage(5, 3);
    SetPixel(image, 1, 0, 255, 255, 255);
    SetPixel(image, 2, 1, 255, 0, 255);
    SetPixel(image, 3, 2, 0, 255, 0);
    const std::vector<std::uint8_t> png = WriteAndRead(image);
    ASSERT_GT(png.size(), 33U);
    EXPECT_EQ(png[1], 'P');
    EXPECT_EQ(png[12], 'I');
    EXPECT_EQ(png[19], 5);
    EXPECT_EQ(png[23], 3);
    EXPECT_EQ(png[24], 2);
    EXPECT_EQ(png[25], 3);
}

TEST(ImageTest, ManyColourImageIsTruecolourPng)
{
    vfdt::Image image = vfdt::MakeImage(20, 20);
    for (int y = 0; y < image.height; ++y)
    {
        for (int x = 0; x < image.width; ++x)
        {
            SetPixel(image, x, y, static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), 0);
        }
    }
    const std::vector<std::uint8_t> png = WriteAndRead(image);
    ASSERT_GT(png.size(), 33U);
    EXPECT_EQ(png[24], 8);
    EXPECT_EQ(png[25], 2);
}
