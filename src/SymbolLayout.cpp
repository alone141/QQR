#include "SymbolLayout.hpp"
#include "Galois.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace qqr
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

std::vector<std::uint8_t> RepairCoefficients(std::uint32_t sessionId, std::uint32_t blockIndex, std::uint32_t esi, std::uint32_t blockSymbols)
{
    std::vector<std::uint8_t> coefficients(blockSymbols, 0);
    if (esi < cauchy_symbol_limit)
    {
        // Cauchy row 1 / (x + y) with x = esi and y = source index; distinct because esi >= blockSymbols.
        for (std::uint32_t k = 0; k < blockSymbols; ++k)
        {
            coefficients[k] = GfInverse(static_cast<std::uint8_t>(esi ^ k));
        }
        return coefficients;
    }

    std::uint64_t state = (static_cast<std::uint64_t>(sessionId) << 32) | blockIndex;
    state = SplitMix(state) ^ (static_cast<std::uint64_t>(esi) * 0xD1B54A32D192ED03ULL);
    std::uint64_t bits = 0;
    for (std::uint32_t k = 0; k < blockSymbols; ++k)
    {
        if (k % 64 == 0)
        {
            bits = SplitMix(state);
        }
        coefficients[k] = static_cast<std::uint8_t>((bits >> (k % 64)) & 1ULL);
    }
    if (std::all_of(coefficients.begin(), coefficients.end(), [](std::uint8_t value) { return value == 0; }))
    {
        coefficients[esi % blockSymbols] = 1;
    }
    return coefficients;
}

} // namespace qqr
