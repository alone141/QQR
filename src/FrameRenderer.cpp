#include "FrameRenderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

FrameRenderer::FrameRenderer(const FrameLayout& layout, int cellPixels)
    : layout{layout}
    , cellPixels{cellPixels}
    , codec{rs_parity_bytes}
{
}

Image FrameRenderer::Render(std::span<const std::uint8_t> payload, std::uint32_t frameNumber) const
{
    const std::vector<std::uint8_t> frameBytes = EncodeFrameBytes(payload);
    const int gridWidth = layout.GridWidth();
    const int gridHeight = layout.GridHeight();
    const int bitsPerCell = layout.BitsPerCell();
    const std::size_t cellsPerByte = 8 / static_cast<std::size_t>(bitsPerCell);
    const unsigned valueMask = (1U << bitsPerCell) - 1;

    std::vector<int> colors(static_cast<std::size_t>(gridWidth) * static_cast<std::size_t>(gridHeight), 0);
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            colors[static_cast<std::size_t>(y) * static_cast<std::size_t>(gridWidth) + static_cast<std::size_t>(x)] =
                layout.ReservedColor(x, y, frameNumber);
        }
    }

    const std::span<const CellPoint> dataCells = layout.DataCells();
    for (std::size_t i = 0; i < dataCells.size(); ++i)
    {
        const std::size_t byteIndex = i / cellsPerByte;
        unsigned value = 0;
        if (byteIndex < frameBytes.size())
        {
            const unsigned shift = static_cast<unsigned>(8 - bitsPerCell * static_cast<int>(i % cellsPerByte + 1));
            value = (frameBytes[byteIndex] >> shift) & valueMask;
        }
        else
        {
            // Cells past the last codeword carry a fixed pseudo-random fill so the frame has no flat areas.
            value = static_cast<unsigned>((i * 2654435761ULL) >> 13) & valueMask;
        }
        const CellPoint cell = dataCells[i];
        colors[static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(gridWidth) + static_cast<std::size_t>(cell.x)] =
            static_cast<int>(value);
    }

    // The quiet zone is a white border that keeps the finders apart from whatever surrounds the screen.
    const int border = quiet_zone_cells * cellPixels;
    Image image = MakeImage((gridWidth + 2 * quiet_zone_cells) * cellPixels, (gridHeight + 2 * quiet_zone_cells) * cellPixels);
    const Rgb white = layout.PaletteColor(palette_white);
    for (int y = 0; y < image.height; ++y)
    {
        const int cellY = (y - border) / cellPixels;
        const bool insideY = y >= border && cellY < gridHeight;
        std::uint8_t* row = image.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) * 3;
        for (int x = 0; x < image.width; ++x)
        {
            const int cellX = (x - border) / cellPixels;
            const bool inside = insideY && x >= border && cellX < gridWidth;
            const Rgb color = inside ? layout.PaletteColor(colors[static_cast<std::size_t>(cellY) * static_cast<std::size_t>(gridWidth) +
                                                                  static_cast<std::size_t>(cellX)])
                                     : white;
            row[3 * x] = color.r;
            row[3 * x + 1] = color.g;
            row[3 * x + 2] = color.b;
        }
    }
    return image;
}

std::vector<std::uint8_t> FrameRenderer::EncodeFrameBytes(std::span<const std::uint8_t> payload) const
{
    const std::size_t codewords = static_cast<std::size_t>(layout.CodewordCount());
    std::vector<std::uint8_t> padded(layout.PayloadBytes(), 0);
    std::copy_n(payload.begin(), std::min(payload.size(), padded.size()), padded.begin());

    // Byte j of codeword c sits at frame byte j * codewords + c, so every codeword is spread across the whole frame.
    std::vector<std::uint8_t> frameBytes(codewords * rs_codeword_bytes, 0);
    std::vector<std::uint8_t> codeword(rs_codeword_bytes, 0);
    for (std::size_t c = 0; c < codewords; ++c)
    {
        const std::span<const std::uint8_t> data{padded.data() + c * rs_data_bytes, static_cast<std::size_t>(rs_data_bytes)};
        std::copy(data.begin(), data.end(), codeword.begin());
        codec.Encode(data, std::span<std::uint8_t>{codeword.data() + rs_data_bytes, static_cast<std::size_t>(rs_parity_bytes)});
        for (std::size_t j = 0; j < rs_codeword_bytes; ++j)
        {
            frameBytes[j * codewords + c] = codeword[j];
        }
    }
    return frameBytes;
}

} // namespace qqr
