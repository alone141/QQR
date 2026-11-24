#pragma once

#include "Packet.hpp"
#include "SymbolLayout.hpp"

#include <cstdint>
#include <span>

namespace qqr
{

class FountainEncoder
{
public:
    /**
     * @brief Prepare to encode @p content. The encoder keeps a view, so the bytes must outlive it.
     * @param content Bytes to transfer. Must not be empty.
     * @param symbolSize Bytes per symbol. Must be greater than zero.
     * @param sessionId Session id written into every packet and used to derive repair masks.
     */
    FountainEncoder(std::span<const std::uint8_t> content, std::uint32_t symbolSize, std::uint32_t sessionId);

    /**
     * @brief Return the block layout of the content.
     * @return The layout.
     */
    const SymbolLayout& Layout() const;

    /**
     * @brief Build one encoded packet. Ids below the block's symbol count are source symbols; higher ids are repair symbols.
     * @param blockIndex Block to encode. Must be below Layout().BlockCount().
     * @param esi Encoding symbol id.
     * @return The packet.
     */
    Packet MakePacket(std::uint32_t blockIndex, std::uint32_t esi) const;

private:
    void AddSourceSymbol(std::uint32_t blockIndex, std::uint32_t symbolIndex, std::uint8_t factor, std::span<std::uint8_t> out) const;

    std::span<const std::uint8_t> content;
    SymbolLayout layout;
    std::uint32_t sessionId;
};

} // namespace qqr
