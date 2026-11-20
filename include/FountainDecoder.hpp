#pragma once

#include "Packet.hpp"
#include "SymbolLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace vfdt
{

constexpr std::uint64_t max_content_bytes = 64ULL * 1024 * 1024 * 1024;

enum class AddCode
{
    Ok,
    Duplicate,
    BlockComplete,
    SessionMismatch,
    OutOfRange,
};

class FountainDecoder
{
public:
    /**
     * @brief Start with no session. The first accepted packet fixes the session.
     */
    FountainDecoder();

    /**
     * @brief Add one received packet and solve its block when enough symbols have arrived.
     * @param packet A packet that passed its checksum.
     * @return AddCode::Ok when the packet was stored or completed its block, otherwise why it was ignored.
     */
    AddCode AddPacket(const Packet& packet);

    /**
     * @brief Report whether a session has been fixed by a packet.
     * @return true after the first accepted packet.
     */
    bool HasSession() const;

    /**
     * @brief Return the block count of the session.
     * @return Block count, or 0 before the first packet.
     */
    std::uint32_t BlockCount() const;

    /**
     * @brief Return how many blocks are fully recovered.
     * @return Completed block count.
     */
    std::uint32_t CompletedBlocks() const;

    /**
     * @brief Report whether every block is recovered.
     * @return true when the content is complete.
     */
    bool IsComplete() const;

    /**
     * @brief List the blocks that are not recovered yet.
     * @return Indexes of incomplete blocks in increasing order.
     */
    std::vector<std::uint32_t> MissingBlocks() const;

    /**
     * @brief Return the recovered content. Only meaningful when IsComplete() is true.
     * @return A view of the content buffer.
     */
    std::span<const std::uint8_t> Content() const;

private:
    struct BlockState
    {
        std::map<std::uint32_t, std::vector<std::uint8_t>> symbols;
        std::size_t nextAttempt{};
        int failedAttempts{};
        bool complete{};
    };

    bool TrySolve(std::uint32_t blockIndex);

    std::optional<SymbolLayout> layout;
    std::uint32_t sessionId;
    std::vector<std::uint8_t> content;
    std::vector<BlockState> blocks;
    std::uint32_t completedBlocks;
};

} // namespace vfdt
