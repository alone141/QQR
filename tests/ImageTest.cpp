#include <gtest/gtest.h>

#include "Image.hpp"

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

} // namespace

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
