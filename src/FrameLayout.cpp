#include "FrameLayout.hpp"
#include "Packet.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace qqr
{

namespace
{

constexpr std::array<Rgb, 4> palette_colors{
    Rgb{0, 0, 0},
    Rgb{255, 255, 255},
    Rgb{255, 0, 255},
    Rgb{0, 255, 0},
};

bool InStripRange(int x, int gridWidth)
{
    return x >= corner_cells && x < gridWidth - corner_cells;
}

} // namespace

FrameLayout::FrameLayout()
    : gridWidth{0}
    , gridHeight{0}
    , bitsPerCell{0}
    , dataCells{}
    , codewordCount{0}
    , packetsPerFrame{0}
    , packetBytes{0}
{
}

LayoutCode FrameLayout::Configure(int newGridWidth, int newGridHeight, int newBitsPerCell)
{
    if (newBitsPerCell != 1 && newBitsPerCell != 2)
    {
        return LayoutCode::UnsupportedBitsPerCell;
    }
    if (newGridWidth % 2 != 0 || newGridHeight % 2 != 0)
    {
        return LayoutCode::OddGrid;
    }
    if (newGridWidth < min_grid_cells || newGridHeight < min_grid_cells)
    {
        return LayoutCode::GridTooSmall;
    }

    gridWidth = newGridWidth;
    gridHeight = newGridHeight;
    bitsPerCell = newBitsPerCell;
    dataCells.clear();
    for (int y = 0; y < gridHeight; ++y)
    {
        for (int x = 0; x < gridWidth; ++x)
        {
            if (RoleAt(x, y) == CellRole::Data)
            {
                dataCells.push_back(CellPoint{x, y});
            }
        }
    }

    const std::size_t dataBytes = dataCells.size() * static_cast<std::size_t>(bitsPerCell) / 8;
    codewordCount = static_cast<int>(dataBytes / rs_codeword_bytes);
    const std::size_t payload = PayloadBytes();
    packetsPerFrame = static_cast<int>(std::max<std::size_t>(1, payload / target_packet_bytes));
    packetBytes = payload / static_cast<std::size_t>(packetsPerFrame);
    if (codewordCount < 1 || packetBytes < packet_overhead_bytes + min_symbol_bytes)
    {
        *this = FrameLayout{};
        return LayoutCode::GridTooSmall;
    }
    return LayoutCode::Ok;
}

int FrameLayout::GridWidth() const
{
    return gridWidth;
}

int FrameLayout::GridHeight() const
{
    return gridHeight;
}

int FrameLayout::BitsPerCell() const
{
    return bitsPerCell;
}

int FrameLayout::ColorCount() const
{
    return 1 << bitsPerCell;
}

CellRole FrameLayout::RoleAt(int x, int y) const
{
    const bool left = x < corner_cells;
    const bool right = x >= gridWidth - corner_cells;
    const bool top = y < corner_cells;
    const bool bottom = y >= gridHeight - corner_cells;
    if ((left || right) && (top || bottom))
    {
        const int finderX = left ? x : x - (gridWidth - finder_cells);
        const int finderY = top ? y : y - (gridHeight - finder_cells);
        const bool inFinder = finderX >= 0 && finderX < finder_cells && finderY >= 0 && finderY < finder_cells;
        return inFinder ? CellRole::Finder : CellRole::Separator;
    }

    const bool stripX = InStripRange(x, gridWidth);
    const bool stripY = y >= corner_cells && y < gridHeight - corner_cells;
    if ((y == finder_center_cell && stripX) || (x == finder_center_cell && stripY))
    {
        return CellRole::Timing;
    }
    if ((y == palette_row_offset || y == gridHeight - 1 - palette_row_offset) && stripX)
    {
        return CellRole::Palette;
    }
    if ((y == parity_row_offset || y == gridHeight - 1 - parity_row_offset) && stripX)
    {
        return CellRole::Parity;
    }
    return CellRole::Data;
}

int FrameLayout::ReservedColor(int x, int y, std::uint32_t frameNumber) const
{
    switch (RoleAt(x, y))
    {
    case CellRole::Finder:
    {
        const int finderX = x < corner_cells ? x : x - (gridWidth - finder_cells);
        const int finderY = y < corner_cells ? y : y - (gridHeight - finder_cells);
        const int moduleX = finderX / finder_module_cells;
        const int moduleY = finderY / finder_module_cells;
        const int ring = std::max(std::abs(moduleX - 3), std::abs(moduleY - 3));
        return ring == 2 ? palette_white : palette_black;
    }
    case CellRole::Timing:
    {
        const int position = y == finder_center_cell ? x : y;
        return position % 2 == 0 ? palette_black : palette_white;
    }
    case CellRole::Palette:
        return ((x - corner_cells) / palette_run_cells) % ColorCount();
    case CellRole::Parity:
        return frameNumber % 2 == 0 ? palette_black : palette_white;
    case CellRole::Separator:
    case CellRole::Data:
        break;
    }
    return palette_white;
}

std::span<const CellPoint> FrameLayout::DataCells() const
{
    return dataCells;
}

int FrameLayout::CodewordCount() const
{
    return codewordCount;
}

std::size_t FrameLayout::PayloadBytes() const
{
    return static_cast<std::size_t>(codewordCount) * rs_data_bytes;
}

int FrameLayout::PacketsPerFrame() const
{
    return packetsPerFrame;
}

std::size_t FrameLayout::PacketBytes() const
{
    return packetBytes;
}

std::uint32_t FrameLayout::SymbolSize() const
{
    return static_cast<std::uint32_t>(packetBytes - packet_overhead_bytes);
}

Rgb FrameLayout::PaletteColor(int index) const
{
    return palette_colors[static_cast<std::size_t>(index)];
}

int GridCells(int screenPixels, int cellPixels)
{
    return std::max(0, screenPixels / cellPixels - 2 * quiet_zone_cells) & ~1;
}

} // namespace qqr
