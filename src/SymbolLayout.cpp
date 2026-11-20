#include "SymbolLayout.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace vfdt
{

namespace
{

std::uint64_t SplitMix(std::uint64_t& state)
{
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

} // namespace

SymbolLayout::SymbolLayout(std::uint64_t contentSize, std::uint32_t symbolSize)
    : contentSize{contentSize}
    , symbolSize{symbolSize}
    , blockCount{0}
{
    const std::uint64_t fullBlockBytes = static_cast<std::uint64_t>(max_block_symbols) * symbolSize;
    blockCount = static_cast<std::uint32_t>((contentSize + fullBlockBytes - 1) / fullBlockBytes);
}

std::uint64_t SymbolLayout::ContentSize() const
{
    return contentSize;
}

std::uint32_t SymbolLayout::SymbolSize() const
{
    return symbolSize;
}

std::uint32_t SymbolLayout::BlockCount() const
{
    return blockCount;
}

std::uint32_t SymbolLayout::BlockSymbols(std::uint32_t blockIndex) const
{
    return static_cast<std::uint32_t>((BlockBytes(blockIndex) + symbolSize - 1) / symbolSize);
}

std::uint64_t SymbolLayout::BlockOffset(std::uint32_t blockIndex) const
{
    return static_cast<std::uint64_t>(blockIndex) * max_block_symbols * symbolSize;
}

std::uint64_t SymbolLayout::BlockBytes(std::uint32_t blockIndex) const
{
    const std::uint64_t fullBlockBytes = static_cast<std::uint64_t>(max_block_symbols) * symbolSize;
    return std::min(fullBlockBytes, contentSize - BlockOffset(blockIndex));
}

std::vector<std::uint64_t> RepairMask(std::uint32_t sessionId, std::uint32_t blockIndex, std::uint32_t esi, std::uint32_t blockSymbols)
{
    std::uint64_t state = (static_cast<std::uint64_t>(sessionId) << 32) | blockIndex;
    state = SplitMix(state) ^ (static_cast<std::uint64_t>(esi) * 0xD1B54A32D192ED03ULL);

    std::vector<std::uint64_t> mask((blockSymbols + 63) / 64, 0);
    for (std::uint64_t& word : mask)
    {
        word = SplitMix(state);
    }
    const std::uint32_t tailBits = blockSymbols % 64;
    if (tailBits != 0)
    {
        mask.back() &= (1ULL << tailBits) - 1;
    }
    if (std::all_of(mask.begin(), mask.end(), [](std::uint64_t word) { return word == 0; }))
    {
        const std::uint32_t bit = esi % blockSymbols;
        mask[bit / 64] |= 1ULL << (bit % 64);
    }
    return mask;
}

} // namespace vfdt
