#include "FountainEncoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdt
{

FountainEncoder::FountainEncoder(std::span<const std::uint8_t> content, std::uint32_t symbolSize, std::uint32_t sessionId)
    : content{content}
    , layout{content.size(), symbolSize}
    , sessionId{sessionId}
{
}

const SymbolLayout& FountainEncoder::Layout() const
{
    return layout;
}

Packet FountainEncoder::MakePacket(std::uint32_t blockIndex, std::uint32_t esi) const
{
    Packet packet{};
    packet.sessionId = sessionId;
    packet.contentSize = layout.ContentSize();
    packet.blockIndex = blockIndex;
    packet.esi = esi;
    packet.symbol.assign(layout.SymbolSize(), 0);

    const std::uint32_t blockSymbols = layout.BlockSymbols(blockIndex);
    if (esi < blockSymbols)
    {
        XorSourceSymbol(blockIndex, esi, packet.symbol);
        return packet;
    }

    const std::vector<std::uint64_t> mask = RepairMask(sessionId, blockIndex, esi, blockSymbols);
    for (std::uint32_t i = 0; i < blockSymbols; ++i)
    {
        if (((mask[i / 64] >> (i % 64)) & 1ULL) != 0)
        {
            XorSourceSymbol(blockIndex, i, packet.symbol);
        }
    }
    return packet;
}

void FountainEncoder::XorSourceSymbol(std::uint32_t blockIndex, std::uint32_t symbolIndex, std::span<std::uint8_t> out) const
{
    const std::uint64_t symbolOffset = static_cast<std::uint64_t>(symbolIndex) * layout.SymbolSize();
    const std::uint64_t blockBytes = layout.BlockBytes(blockIndex);
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(layout.SymbolSize(), blockBytes - symbolOffset));
    const std::uint8_t* source = content.data() + layout.BlockOffset(blockIndex) + symbolOffset;
    for (std::size_t i = 0; i < available; ++i)
    {
        out[i] ^= source[i];
    }
}

} // namespace vfdt
