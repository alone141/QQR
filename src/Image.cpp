#include "Image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace vfdt
{

Image MakeImage(int width, int height)
{
    Image image{};
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return image;
}

std::array<double, 3> SampleBilinear(const Image& image, double x, double y)
{
    const double fx = x - 0.5;
    const double fy = y - 0.5;
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, image.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, image.height - 1);
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const double ax = std::clamp(fx - x0, 0.0, 1.0);
    const double ay = std::clamp(fy - y0, 0.0, 1.0);
    const std::size_t width = static_cast<std::size_t>(image.width);
    const std::uint8_t* p00 = image.pixels.data() + (static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x0)) * 3;
    const std::uint8_t* p10 = image.pixels.data() + (static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x1)) * 3;
    const std::uint8_t* p01 = image.pixels.data() + (static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x0)) * 3;
    const std::uint8_t* p11 = image.pixels.data() + (static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x1)) * 3;
    std::array<double, 3> color{};
    for (std::size_t c = 0; c < 3; ++c)
    {
        const double top = p00[c] * (1.0 - ax) + p10[c] * ax;
        const double bottom = p01[c] * (1.0 - ax) + p11[c] * ax;
        color[c] = top * (1.0 - ay) + bottom * ay;
    }
    return color;
}

ImageIoCode WritePpm(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file{path, std::ios::binary};
    if (!file)
    {
        return ImageIoCode::OpenFailed;
    }
    const std::string header = "P6\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
    file.write(header.data(), static_cast<std::streamsize>(header.size()));
    file.write(reinterpret_cast<const char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
    return file ? ImageIoCode::Ok : ImageIoCode::WriteFailed;
}

} // namespace vfdt
