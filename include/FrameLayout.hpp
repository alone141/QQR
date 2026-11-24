#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

constexpr int finder_module_cells = 3;
constexpr int finder_cells = 7 * finder_module_cells;
constexpr int corner_cells = finder_cells + finder_module_cells;
constexpr int finder_center_cell = finder_cells / 2;
constexpr int quiet_zone_cells = finder_module_cells;
constexpr int palette_row_offset = 4;
constexpr int parity_row_offset = 16;
constexpr int palette_run_cells = 3;
constexpr int min_grid_cells = 64;
constexpr int rs_codeword_bytes = 255;
constexpr int rs_parity_bytes = 64;
constexpr int rs_data_bytes = rs_codeword_bytes - rs_parity_bytes;
constexpr std::size_t target_packet_bytes = 1024;
constexpr std::size_t min_symbol_bytes = 16;
constexpr int palette_black = 0;
constexpr int palette_white = 1;

struct Rgb
{
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
};

struct CellPoint
{
    int x{};
    int y{};
};

enum class CellRole
{
    Data,
    Finder,
    Separator,
    Timing,
    Palette,
    Parity,
};

enum class LayoutCode
{
    Ok,
    OddGrid,
    GridTooSmall,
    UnsupportedBitsPerCell,
};

class FrameLayout
{
public:
    /**
     * @brief Create an unconfigured layout. Call Configure before use.
     */
    FrameLayout();

    /**
     * @brief Lay out a grid: corner finders, timing tracks, palette and parity strips, data cells.
     * @param gridWidth Cells across. Must be even and at least min_grid_cells.
     * @param gridHeight Cells down. Must be even and at least min_grid_cells.
     * @param bitsPerCell 1 (two colours) or 2 (four colours).
     * @return LayoutCode::Ok on success, otherwise why the grid cannot be used.
     */
    LayoutCode Configure(int gridWidth, int gridHeight, int bitsPerCell);

    /**
     * @brief Return the grid width.
     * @return Cells across, or 0 before Configure.
     */
    int GridWidth() const;

    /**
     * @brief Return the grid height.
     * @return Cells down, or 0 before Configure.
     */
    int GridHeight() const;

    /**
     * @brief Return the bits each data cell carries.
     * @return 1 or 2.
     */
    int BitsPerCell() const;

    /**
     * @brief Return the number of colours data cells use.
     * @return 2 or 4.
     */
    int ColorCount() const;

    /**
     * @brief Classify one cell.
     * @param x Column, inside the grid.
     * @param y Row, inside the grid.
     * @return What the cell is used for.
     */
    CellRole RoleAt(int x, int y) const;

    /**
     * @brief Return the palette index a non-data cell shows.
     * @param x Column, inside the grid.
     * @param y Row, inside the grid.
     * @param frameNumber Frame being drawn; selects the parity strip colour.
     * @return Palette index of the cell.
     */
    int ReservedColor(int x, int y, std::uint32_t frameNumber) const;

    /**
     * @brief Return the data cells in the order bits are written.
     * @return Data cells, row-major.
     */
    std::span<const CellPoint> DataCells() const;

    /**
     * @brief Return how many Reed-Solomon codewords fit in one frame.
     * @return Codewords per frame.
     */
    int CodewordCount() const;

    /**
     * @brief Return the payload bytes one frame carries after error correction.
     * @return CodewordCount() * rs_data_bytes.
     */
    std::size_t PayloadBytes() const;

    /**
     * @brief Return how many packets one frame carries.
     * @return Packets per frame.
     */
    int PacketsPerFrame() const;

    /**
     * @brief Return the size of one serialized packet.
     * @return Bytes per packet.
     */
    std::size_t PacketBytes() const;

    /**
     * @brief Return the fountain symbol size that fills one packet.
     * @return Bytes per symbol.
     */
    std::uint32_t SymbolSize() const;

    /**
     * @brief Return the drawn colour of a palette index.
     * @param index Palette index, below ColorCount().
     * @return The colour.
     */
    Rgb PaletteColor(int index) const;

private:
    int gridWidth;
    int gridHeight;
    int bitsPerCell;
    std::vector<CellPoint> dataCells;
    int codewordCount;
    int packetsPerFrame;
    std::size_t packetBytes;
};

/**
 * @brief Return the even grid cell count that fits a screen dimension, leaving room for the quiet zone.
 * @param screenPixels Screen width or height in pixels.
 * @param cellPixels Cell size in pixels. Must be greater than zero.
 * @return Largest even count with (count + 2 * quiet_zone_cells) * cellPixels <= screenPixels.
 */
int GridCells(int screenPixels, int cellPixels);

} // namespace qqr
