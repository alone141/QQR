#include "Image.hpp"
#include "Checksum.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vfdt
{

namespace
{

constexpr std::array<std::uint8_t, 8> png_signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr std::size_t max_stored_block = 65535;
constexpr std::uint32_t adler_modulus = 65521;
constexpr std::size_t max_palette_colors = 256;

void PutBigEndian(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void AppendChunk(std::vector<std::uint8_t>& file, std::string_view type, std::span<const std::uint8_t> data)
{
    PutBigEndian(file, static_cast<std::uint32_t>(data.size()));
    const std::size_t typeStart = file.size();
    file.insert(file.end(), type.begin(), type.end());
    file.insert(file.end(), data.begin(), data.end());
    PutBigEndian(file, Crc32(std::span<const std::uint8_t>{file.data() + typeStart, file.size() - typeStart}));
}

// A zlib stream made of uncompressed (stored) deflate blocks: no compressor needed, still a valid PNG.
std::vector<std::uint8_t> StoreZlib(std::span<const std::uint8_t> raw)
{
    std::vector<std::uint8_t> out{0x78, 0x01};
    std::size_t offset = 0;
    do
    {
        const std::size_t length = std::min(max_stored_block, raw.size() - offset);
        const bool last = offset + length == raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(~length));
        out.push_back(static_cast<std::uint8_t>(~length >> 8));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset), raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    } while (offset < raw.size());

    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : raw)
    {
        a = (a + byte) % adler_modulus;
        b = (b + a) % adler_modulus;
    }
    PutBigEndian(out, (b << 16) | a);
    return out;
}

std::uint32_t PackRgb(const std::uint8_t* pixel)
{
    return (static_cast<std::uint32_t>(pixel[0]) << 16) | (static_cast<std::uint32_t>(pixel[1]) << 8) | pixel[2];
}

} // namespace

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

ImageIoCode WritePng(const std::filesystem::path& path, const Image& image)
{
    const std::size_t pixelCount = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    std::map<std::uint32_t, std::uint8_t> palette;
    for (std::size_t i = 0; i < pixelCount && palette.size() <= max_palette_colors; ++i)
    {
        palette.emplace(PackRgb(image.pixels.data() + 3 * i), static_cast<std::uint8_t>(0));
    }
    const bool indexed = palette.size() <= max_palette_colors;
    int bitDepth = 8;
    if (indexed)
    {
        std::uint8_t next = 0;
        for (auto& entry : palette)
        {
            entry.second = next++;
        }
        bitDepth = palette.size() <= 2 ? 1 : palette.size() <= 4 ? 2 : palette.size() <= 16 ? 4 : 8;
    }

    const std::size_t width = static_cast<std::size_t>(image.width);
    const std::size_t rowBytes = indexed ? (width * static_cast<std::size_t>(bitDepth) + 7) / 8 : width * 3;
    std::vector<std::uint8_t> raw((rowBytes + 1) * static_cast<std::size_t>(image.height), 0);
    for (std::size_t y = 0; y < static_cast<std::size_t>(image.height); ++y)
    {
        std::uint8_t* row = raw.data() + y * (rowBytes + 1) + 1;
        const std::uint8_t* source = image.pixels.data() + y * width * 3;
        if (!indexed)
        {
            std::copy(source, source + width * 3, row);
            continue;
        }
        const std::size_t perByte = 8 / static_cast<std::size_t>(bitDepth);
        for (std::size_t x = 0; x < width; ++x)
        {
            const unsigned index = palette.at(PackRgb(source + 3 * x));
            const unsigned shift = static_cast<unsigned>(8 - bitDepth * static_cast<int>(x % perByte + 1));
            row[x / perByte] |= static_cast<std::uint8_t>(index << shift);
        }
    }

    std::vector<std::uint8_t> header;
    PutBigEndian(header, static_cast<std::uint32_t>(image.width));
    PutBigEndian(header, static_cast<std::uint32_t>(image.height));
    header.push_back(static_cast<std::uint8_t>(bitDepth));
    header.push_back(indexed ? 3 : 2);
    header.push_back(0);
    header.push_back(0);
    header.push_back(0);

    std::vector<std::uint8_t> file(png_signature.begin(), png_signature.end());
    AppendChunk(file, "IHDR", header);
    if (indexed)
    {
        std::vector<std::uint8_t> colors;
        for (const auto& [rgb, index] : palette)
        {
            colors.push_back(static_cast<std::uint8_t>(rgb >> 16));
            colors.push_back(static_cast<std::uint8_t>(rgb >> 8));
            colors.push_back(static_cast<std::uint8_t>(rgb));
        }
        AppendChunk(file, "PLTE", colors);
    }
    AppendChunk(file, "IDAT", StoreZlib(raw));
    AppendChunk(file, "IEND", {});

    std::ofstream out{path, std::ios::binary};
    if (!out)
    {
        return ImageIoCode::OpenFailed;
    }
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return out ? ImageIoCode::Ok : ImageIoCode::WriteFailed;
}

} // namespace vfdt
