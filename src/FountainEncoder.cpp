#include "FountainEncoder.hpp"
#include "Galois.hpp"

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
        AddSourceSymbol(blockIndex, esi, 1, packet.symbol);
        return packet;
    }

    const std::vector<std::uint8_t> coefficients = RepairCoefficients(sessionId, blockIndex, esi, blockSymbols);
    for (std::uint32_t i = 0; i < blockSymbols; ++i)
    {
        AddSourceSymbol(blockIndex, i, coefficients[i], packet.symbol);
    }
    return packet;
}

void FountainEncoder::AddSourceSymbol(std::uint32_t blockIndex, std::uint32_t symbolIndex, std::uint8_t factor, std::span<std::uint8_t> out) const
{
    const std::uint64_t symbolOffset = static_cast<std::uint64_t>(symbolIndex) * layout.SymbolSize();
    const std::uint64_t blockBytes = layout.BlockBytes(blockIndex);
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(layout.SymbolSize(), blockBytes - symbolOffset));
    const std::span<const std::uint8_t> source{content.data() + layout.BlockOffset(blockIndex) + symbolOffset, available};
    GfMulAdd(out.first(available), source, factor);
}

} // namespace vfdt
