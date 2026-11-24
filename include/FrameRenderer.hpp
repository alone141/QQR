#pragma once

#include "FrameLayout.hpp"
#include "Image.hpp"
#include "ReedSolomon.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

class FrameRenderer
{
public:
    /**
     * @brief Prepare to draw frames of @p layout.
     * @param layout A configured layout.
     * @param cellPixels Screen pixels per cell side.
     */
    FrameRenderer(const FrameLayout& layout, int cellPixels);

    /**
     * @brief Protect @p payload with Reed-Solomon, spread the codewords over the frame, and draw it.
     * @param payload Up to layout.PayloadBytes() bytes; shorter payloads are zero-padded.
     * @param frameNumber Frame counter; its lowest bit sets the parity strips.
     * @return The frame image: the grid plus a quiet_zone_cells white border, in screen pixels.
     */
    Image Render(std::span<const std::uint8_t> payload, std::uint32_t frameNumber) const;

private:
    std::vector<std::uint8_t> EncodeFrameBytes(std::span<const std::uint8_t> payload) const;

    FrameLayout layout;
    int cellPixels;
    ReedSolomon codec;
};

} // namespace qqr
