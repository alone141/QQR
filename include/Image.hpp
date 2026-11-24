#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace qqr
{

enum class PixelFormat
{
    // Red, green and blue bytes per pixel, row-major.
    kRgb24,
    // Planar, as FFmpeg's yuv420p: the luma plane, then the U and V planes, each half the size both ways (rounded up).
    kYuv420,
};

struct Image
{
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels;
    PixelFormat format{PixelFormat::kRgb24};
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
 * @brief Return the size of a chroma plane along one axis of a planar YUV 4:2:0 image.
 * @param lumaSize Image width or height in pixels.
 * @return Half of @p lumaSize, rounded up.
 */
int ChromaSize(int lumaSize);

/**
 * @brief Create a planar YUV 4:2:0 image with every byte zero.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return The image: width * height luma bytes, then two chroma planes of ChromaSize(width) * ChromaSize(height) bytes.
 */
Image MakeYuv420Image(int width, int height);

/**
 * @brief Sample an RGB image with bilinear interpolation. Pixel i covers [i, i + 1), so its centre is i + 0.5.
 * @param image RGB image to sample. Must not be empty.
 * @param x Horizontal position in pixels; clamped to the image.
 * @param y Vertical position in pixels; clamped to the image.
 * @return Red, green and blue in 0..255.
 */
std::array<double, 3> SampleBilinear(const Image& image, double x, double y);

/**
 * @brief Sample an image of either pixel format with bilinear interpolation, at the same coordinates as SampleBilinear.
 *        In a planar image every pixel takes the chroma of its 2x2 block, and the result is converted as video-range
 *        BT.709.
 * @param image Image to sample. Must not be empty.
 * @param x Horizontal position in pixels; clamped to the image.
 * @param y Vertical position in pixels; clamped to the image.
 * @return Red, green and blue. Converted values are not clamped, so they can fall a little outside 0..255.
 */
std::array<double, 3> SampleColor(const Image& image, double x, double y);

/**
 * @brief Write an image as binary PPM (P6), for inspection.
 * @param path File to write.
 * @param image RGB image to write.
 * @return ImageIoCode::Ok on success, otherwise the failure.
 */
ImageIoCode WritePpm(const std::filesystem::path& path, const Image& image);

/**
 * @brief Write an image as PNG, lossless. Images with at most 256 colours are stored indexed, at the smallest bit depth.
 * @param path File to write.
 * @param image RGB image to write.
 * @return ImageIoCode::Ok on success, otherwise the failure.
 */
ImageIoCode WritePng(const std::filesystem::path& path, const Image& image);

} // namespace qqr
