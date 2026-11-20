#include "FountainDecoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace vfdt
{

namespace
{

constexpr std::size_t extra_rows_per_attempt = 16;
constexpr std::size_t retry_after_symbols = 4;

struct EquationRow
{
    std::vector<std::uint64_t> bits;
    std::vector<std::uint8_t> data;
};

void XorBytes(std::span<std::uint8_t> target, std::span<const std::uint8_t> source)
{
    for (std::size_t i = 0; i < target.size(); ++i)
    {
        target[i] ^= source[i];
    }
}

bool TestBit(const std::vector<std::uint64_t>& bits, std::size_t index)
{
    return ((bits[index / 64] >> (index % 64)) & 1ULL) != 0;
}

} // namespace

FountainDecoder::FountainDecoder()
    : layout{}
    , sessionId{0}
    , content{}
    , blocks{}
    , completedBlocks{0}
{
}

AddCode FountainDecoder::AddPacket(const Packet& packet)
{
    if (!layout.has_value())
    {
        if (packet.symbol.empty() || packet.contentSize == 0 || packet.contentSize > max_content_bytes)
        {
            return AddCode::OutOfRange;
        }
        layout.emplace(packet.contentSize, static_cast<std::uint32_t>(packet.symbol.size()));
        sessionId = packet.sessionId;
        content.assign(static_cast<std::size_t>(packet.contentSize), 0);
        blocks.assign(layout->BlockCount(), BlockState{});
    }
    else if (packet.sessionId != sessionId || packet.contentSize != layout->ContentSize() ||
             packet.symbol.size() != layout->SymbolSize())
    {
        return AddCode::SessionMismatch;
    }

    if (packet.blockIndex >= layout->BlockCount())
    {
        return AddCode::OutOfRange;
    }

    BlockState& block = blocks[packet.blockIndex];
    if (block.complete)
    {
        return AddCode::BlockComplete;
    }
    if (!block.symbols.emplace(packet.esi, packet.symbol).second)
    {
        return AddCode::Duplicate;
    }

    const std::size_t blockSymbols = layout->BlockSymbols(packet.blockIndex);
    if (block.symbols.size() >= std::max(blockSymbols, block.nextAttempt))
    {
        if (TrySolve(packet.blockIndex))
        {
            block.complete = true;
            block.symbols.clear();
            ++completedBlocks;
        }
        else
        {
            ++block.failedAttempts;
            block.nextAttempt = block.symbols.size() + retry_after_symbols;
        }
    }
    return AddCode::Ok;
}

bool FountainDecoder::HasSession() const
{
    return layout.has_value();
}

std::uint32_t FountainDecoder::BlockCount() const
{
    return layout.has_value() ? layout->BlockCount() : 0;
}

std::uint32_t FountainDecoder::CompletedBlocks() const
{
    return completedBlocks;
}

bool FountainDecoder::IsComplete() const
{
    return layout.has_value() && completedBlocks == layout->BlockCount();
}

std::vector<std::uint32_t> FountainDecoder::MissingBlocks() const
{
    std::vector<std::uint32_t> missing;
    for (std::uint32_t i = 0; i < blocks.size(); ++i)
    {
        if (!blocks[i].complete)
        {
            missing.push_back(i);
        }
    }
    return missing;
}

std::span<const std::uint8_t> FountainDecoder::Content() const
{
    return content;
}

bool FountainDecoder::TrySolve(std::uint32_t blockIndex)
{
    BlockState& block = blocks[blockIndex];
    const std::uint32_t blockSymbols = layout->BlockSymbols(blockIndex);
    const std::size_t symbolSize = layout->SymbolSize();

    std::vector<const std::vector<std::uint8_t>*> known(blockSymbols, nullptr);
    std::vector<std::uint32_t> repairIds;
    for (const auto& [esi, symbol] : block.symbols)
    {
        if (esi < blockSymbols)
        {
            known[esi] = &symbol;
        }
        else
        {
            repairIds.push_back(esi);
        }
    }

    std::vector<std::uint32_t> unknown;
    std::vector<std::size_t> unknownSlot(blockSymbols, 0);
    for (std::uint32_t i = 0; i < blockSymbols; ++i)
    {
        if (known[i] == nullptr)
        {
            unknownSlot[i] = unknown.size();
            unknown.push_back(i);
        }
    }

    std::vector<std::vector<std::uint8_t>> solved(unknown.size());
    if (!unknown.empty())
    {
        if (repairIds.size() < unknown.size())
        {
            return false;
        }

        const std::size_t extraRows = extra_rows_per_attempt * (static_cast<std::size_t>(block.failedAttempts) + 1);
        const std::size_t rowCount = std::min(repairIds.size(), unknown.size() + extraRows);
        const std::size_t words = (unknown.size() + 63) / 64;

        // Each repair row starts as mask * sources = symbol; known sources move to the right side.
        std::vector<EquationRow> rows(rowCount);
        for (std::size_t r = 0; r < rowCount; ++r)
        {
            const std::uint32_t esi = repairIds[r];
            const std::vector<std::uint64_t> mask = RepairMask(sessionId, blockIndex, esi, blockSymbols);
            rows[r].bits.assign(words, 0);
            rows[r].data = block.symbols.at(esi);
            for (std::uint32_t column = 0; column < blockSymbols; ++column)
            {
                if (!TestBit(mask, column))
                {
                    continue;
                }
                if (known[column] != nullptr)
                {
                    XorBytes(rows[r].data, *known[column]);
                }
                else
                {
                    const std::size_t slot = unknownSlot[column];
                    rows[r].bits[slot / 64] |= 1ULL << (slot % 64);
                }
            }
        }

        // Gauss-Jordan elimination over GF(2) on the unknown columns.
        for (std::size_t column = 0; column < unknown.size(); ++column)
        {
            std::size_t pivot = column;
            while (pivot < rowCount && !TestBit(rows[pivot].bits, column))
            {
                ++pivot;
            }
            if (pivot == rowCount)
            {
                return false;
            }
            std::swap(rows[column], rows[pivot]);
            for (std::size_t r = 0; r < rowCount; ++r)
            {
                if (r == column || !TestBit(rows[r].bits, column))
                {
                    continue;
                }
                for (std::size_t w = column / 64; w < words; ++w)
                {
                    rows[r].bits[w] ^= rows[column].bits[w];
                }
                XorBytes(rows[r].data, rows[column].data);
            }
        }
        for (std::size_t i = 0; i < unknown.size(); ++i)
        {
            solved[i] = std::move(rows[i].data);
        }
    }

    const std::uint64_t blockOffset = layout->BlockOffset(blockIndex);
    const std::uint64_t blockBytes = layout->BlockBytes(blockIndex);
    for (std::uint32_t i = 0; i < blockSymbols; ++i)
    {
        const std::vector<std::uint8_t>& symbol = known[i] != nullptr ? *known[i] : solved[unknownSlot[i]];
        const std::uint64_t symbolOffset = static_cast<std::uint64_t>(i) * symbolSize;
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(symbolSize, blockBytes - symbolOffset));
        std::memcpy(content.data() + blockOffset + symbolOffset, symbol.data(), count);
    }
    return true;
}

} // namespace vfdt
