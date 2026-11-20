#include "Transmitter.hpp"
#include "Packet.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdt
{

Transmitter::Transmitter(std::span<const std::uint8_t> content, std::uint32_t sessionId, const FrameLayout& layout, double repairRatio)
    : encoder{content, layout.SymbolSize(), sessionId}
    , layout{layout}
    , windowStarts{0}
    , windowRounds{}
{
    // Blocks go out in windows of window_blocks, interleaved, so the receiver holds only a few blocks at a time.
    const SymbolLayout& symbols = encoder.Layout();
    for (std::uint32_t first = 0; first < symbols.BlockCount(); first += window_blocks)
    {
        const std::uint32_t count = std::min(window_blocks, symbols.BlockCount() - first);
        std::uint32_t largest = 0;
        for (std::uint32_t b = first; b < first + count; ++b)
        {
            largest = std::max(largest, symbols.BlockSymbols(b));
        }
        const std::uint32_t rounds = static_cast<std::uint32_t>(std::ceil(largest * (1.0 + std::max(0.0, repairRatio))));
        windowRounds.push_back(std::max(rounds, largest));
        windowStarts.push_back(windowStarts.back() + static_cast<std::uint64_t>(windowRounds.back()) * count);
    }
}

std::uint64_t Transmitter::PacketsPerPass() const
{
    return windowStarts.back();
}

std::uint64_t Transmitter::FramesPerPass() const
{
    const std::uint64_t perFrame = static_cast<std::uint64_t>(layout.PacketsPerFrame());
    return (PacketsPerPass() + perFrame - 1) / perFrame;
}

std::vector<std::uint8_t> Transmitter::FramePayload(std::uint64_t frameIndex) const
{
    std::vector<std::uint8_t> payload(layout.PayloadBytes(), 0);
    const std::size_t packetBytes = layout.PacketBytes();
    for (int slot = 0; slot < layout.PacketsPerFrame(); ++slot)
    {
        const Slot target = SlotAt(frameIndex * static_cast<std::uint64_t>(layout.PacketsPerFrame()) + static_cast<std::uint64_t>(slot));
        const std::vector<std::uint8_t> bytes = SerializePacket(encoder.MakePacket(target.blockIndex, target.esi));
        std::copy(bytes.begin(), bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(slot) * packetBytes));
    }
    return payload;
}

Transmitter::Slot Transmitter::SlotAt(std::uint64_t packetIndex) const
{
    const std::uint64_t pass = packetIndex / PacketsPerPass();
    const std::uint64_t inPass = packetIndex % PacketsPerPass();
    const std::size_t window = static_cast<std::size_t>(std::upper_bound(windowStarts.begin(), windowStarts.end(), inPass) - windowStarts.begin()) - 1;
    const std::uint64_t local = inPass - windowStarts[window];
    const std::uint32_t first = static_cast<std::uint32_t>(window) * window_blocks;
    const std::uint32_t count = std::min(window_blocks, encoder.Layout().BlockCount() - first);
    Slot slot{};
    slot.blockIndex = first + static_cast<std::uint32_t>(local % count);
    slot.esi = static_cast<std::uint32_t>(local / count + pass * windowRounds[window]);
    return slot;
}

} // namespace vfdt
