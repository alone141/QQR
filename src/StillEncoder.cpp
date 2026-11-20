#include "StillEncoder.hpp"
#include "FountainEncoder.hpp"
#include "Packet.hpp"
#include "SymbolLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdt
{

namespace
{

// Packets of a still go to blocks in turn, so block b gets every BlockCount()-th slot.
std::uint32_t PacketsForBlock(std::uint32_t blockIndex, std::uint32_t blockCount, std::uint32_t packets)
{
    return packets / blockCount + (blockIndex < packets % blockCount ? 1U : 0U);
}

bool FitsInStill(std::uint64_t contentSize, const FrameLayout& layout)
{
    const SymbolLayout symbols{contentSize, layout.SymbolSize()};
    const std::uint32_t packets = static_cast<std::uint32_t>(layout.PacketsPerFrame());
    if (symbols.BlockCount() > packets)
    {
        return false;
    }
    for (std::uint32_t b = 0; b < symbols.BlockCount(); ++b)
    {
        const std::uint32_t sources = symbols.BlockSymbols(b);
        const std::uint32_t repairs = std::max(still_min_repair_symbols, static_cast<std::uint32_t>(std::ceil(sources * still_repair_ratio)));
        if (PacketsForBlock(b, symbols.BlockCount(), packets) < sources + repairs)
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::uint64_t StillCapacity(const FrameLayout& layout)
{
    std::uint64_t low = 0;
    std::uint64_t high = static_cast<std::uint64_t>(layout.PacketsPerFrame()) * layout.SymbolSize();
    while (low < high)
    {
        const std::uint64_t middle = low + (high - low + 1) / 2;
        if (FitsInStill(middle, layout))
        {
            low = middle;
        }
        else
        {
            high = middle - 1;
        }
    }
    return low;
}

StillCode BuildStillPayload(std::span<const std::uint8_t> content, std::uint32_t sessionId, const FrameLayout& layout,
                            std::vector<std::uint8_t>& payload)
{
    if (content.empty() || !FitsInStill(content.size(), layout))
    {
        return StillCode::TooLarge;
    }

    const FountainEncoder encoder{content, layout.SymbolSize(), sessionId};
    const std::uint32_t blockCount = encoder.Layout().BlockCount();
    payload.assign(layout.PayloadBytes(), 0);
    for (int slot = 0; slot < layout.PacketsPerFrame(); ++slot)
    {
        const std::uint32_t index = static_cast<std::uint32_t>(slot);
        const std::vector<std::uint8_t> bytes = SerializePacket(encoder.MakePacket(index % blockCount, index / blockCount));
        std::copy(bytes.begin(), bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(slot) * layout.PacketBytes()));
    }
    return StillCode::Ok;
}

} // namespace vfdt
