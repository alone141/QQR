#pragma once

#include <cstdint>
#include <vector>

namespace vfdt
{

constexpr std::uint32_t max_block_symbols = 1024;
constexpr std::uint32_t cauchy_symbol_limit = 256;

class SymbolLayout
{
public:
    /**
     * @brief Split @p contentSize bytes into blocks of up to max_block_symbols symbols.
     * @param contentSize Total content bytes. Must be greater than zero.
     * @param symbolSize Bytes per symbol. Must be greater than zero.
     */
    SymbolLayout(std::uint64_t contentSize, std::uint32_t symbolSize);

    /**
     * @brief Return the total content size.
     * @return Content bytes.
     */
    std::uint64_t ContentSize() const;

    /**
     * @brief Return the symbol size.
     * @return Bytes per symbol.
     */
    std::uint32_t SymbolSize() const;

    /**
     * @brief Return how many blocks the content is split into.
     * @return Block count.
     */
    std::uint32_t BlockCount() const;

    /**
     * @brief Return the number of source symbols in one block.
     * @param blockIndex Block to query. Must be below BlockCount().
     * @return Source symbols in the block; the last one is zero-padded.
     */
    std::uint32_t BlockSymbols(std::uint32_t blockIndex) const;

    /**
     * @brief Return where a block starts in the content.
     * @param blockIndex Block to query. Must be below BlockCount().
     * @return Byte offset of the block.
     */
    std::uint64_t BlockOffset(std::uint32_t blockIndex) const;

    /**
     * @brief Return how many content bytes a block holds.
     * @param blockIndex Block to query. Must be below BlockCount().
     * @return Content bytes in the block, without padding.
     */
    std::uint64_t BlockBytes(std::uint32_t blockIndex) const;

private:
    std::uint64_t contentSize;
    std::uint32_t symbolSize;
    std::uint32_t blockCount;
};

/**
 * @brief Build the GF(256) coefficients that make repair symbol @p esi from the block's source symbols.
 *        Ids below cauchy_symbol_limit use a Cauchy matrix, so any blockSymbols symbols of a block with fewer
 *        than cauchy_symbol_limit sources recover it. Higher ids use random 0/1 coefficients, which cost only XOR.
 *        Encoder and decoder share it.
 * @param sessionId Session the block belongs to.
 * @param blockIndex Block the repair symbol belongs to.
 * @param esi Encoding symbol id. Must be at least @p blockSymbols; lower ids are the source symbols themselves.
 * @param blockSymbols Source symbols in the block.
 * @return One coefficient per source symbol. Never all zero.
 */
std::vector<std::uint8_t> RepairCoefficients(std::uint32_t sessionId, std::uint32_t blockIndex, std::uint32_t esi, std::uint32_t blockSymbols);

} // namespace vfdt
