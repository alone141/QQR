#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace vfdt
{

struct Image
{
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels;
};

enum class ImageIoCode
{
    Ok,
    OpenFailed,
    WriteFailed,
};

/**
 * @brief Create a black RGB image.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return The image, with 3 bytes per pixel in row-major order.
 */
Image MakeImage(int width, int height);

/**
 * @brief Sample an image with bilinear interpolation. Pixel i covers [i, i + 1), so its centre is i + 0.5.
 * @param image Image to sample. Must not be empty.
 * @param x Horizontal position in pixels; clamped to the image.
 * @param y Vertical position in pixels; clamped to the image.
 * @return Red, green and blue in 0..255.
 */
std::array<double, 3> SampleBilinear(const Image& image, double x, double y);

/**
 * @brief Write an image as binary PPM (P6), for inspection.
 * @param path File to write.
 * @param image Image to write.
 * @return ImageIoCode::Ok on success, otherwise the failure.
 */
ImageIoCode WritePpm(const std::filesystem::path& path, const Image& image);

} // namespace vfdt
