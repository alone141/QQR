#pragma once

#include "FrameLayout.hpp"
#include "Image.hpp"
#include "ReedSolomon.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vfdt
{

enum class ReadCode
{
    Ok,
    FindersNotFound,
    UnsupportedGrid,
    LowContrast,
    BlendedFrame,
    TornFrame,
};

struct FrameRead
{
    int gridWidth{};
    int gridHeight{};
    std::uint32_t parity{};
    std::vector<std::uint8_t> payload;
    std::vector<bool> codewordOk;
    int correctedBytes{};
    int maxCorrectedBytes{};
    int failedCodewords{};
    int packetsPerFrame{};
    std::size_t packetBytes{};
};

class FrameReader
{
public:
    /**
     * @brief Prepare to read frames drawn with @p bitsPerCell bits per data cell.
     * @param bitsPerCell 1 or 2; must match the sender.
     */
    explicit FrameReader(int bitsPerCell);

    /**
     * @brief Locate the frame in a camera image, sample every cell, and correct the codewords.
     * @param image Camera image that shows the whole frame, in either pixel format.
     * @param out Receives the payload and per-codeword status on success.
     * @return ReadCode::Ok when the frame was sampled, otherwise why it was rejected.
     */
    ReadCode ReadFrame(const Image& image, FrameRead& out) const;

private:
    int bitsPerCell;
    ReedSolomon codec;
    // mutable: rebuilding the cell list is costly, and every frame of a session has the same grid.
    mutable FrameLayout cachedLayout;
};

} // namespace vfdt
