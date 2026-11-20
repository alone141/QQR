#pragma once

#include "FountainEncoder.hpp"
#include "FrameLayout.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace vfdt
{

constexpr std::uint32_t window_blocks = 8;

class Transmitter
{
public:
    /**
     * @brief Plan the packet order for @p content. Keeps a view of the content, so it must outlive the transmitter.
     * @param content Packed content (see PackContent). Must not be empty.
     * @param sessionId Session id for every packet.
     * @param layout Configured frame layout; sets the symbol size and packets per frame.
     * @param repairRatio Extra repair symbols per block in one pass, as a share of the block's source symbols.
     */
    Transmitter(std::span<const std::uint8_t> content, std::uint32_t sessionId, const FrameLayout& layout, double repairRatio);

    /**
     * @brief Return the number of packets in one pass over the content.
     * @return Packets per pass.
     */
    std::uint64_t PacketsPerPass() const;

    /**
     * @brief Return the number of frames in one pass over the content.
     * @return Frames per pass, rounded up.
     */
    std::uint64_t FramesPerPass() const;

    /**
     * @brief Build the payload of one frame. Frames past one pass carry fresh repair symbols.
     * @param frameIndex Frame counter from the start of the transmission.
     * @return layout.PayloadBytes() bytes holding layout.PacketsPerFrame() packets.
     */
    std::vector<std::uint8_t> FramePayload(std::uint64_t frameIndex) const;

private:
    struct Slot
    {
        std::uint32_t blockIndex{};
        std::uint32_t esi{};
    };

    Slot SlotAt(std::uint64_t packetIndex) const;

    FountainEncoder encoder;
    FrameLayout layout;
    std::vector<std::uint64_t> windowStarts;
    std::vector<std::uint32_t> windowRounds;
};

} // namespace vfdt
