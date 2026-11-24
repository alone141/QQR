#pragma once

#include "FrameLayout.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

constexpr double still_repair_ratio = 0.25;
constexpr std::uint32_t still_min_repair_symbols = 2;

enum class StillCode
{
    Ok,
    TooLarge,
};

/**
 * @brief Return the largest content one still frame holds, keeping the repair margin for every block.
 * @param layout Configured frame layout.
 * @return Content bytes (packed content, see PackContent), or 0 when nothing fits.
 */
std::uint64_t StillCapacity(const FrameLayout& layout);

/**
 * @brief Fill one frame with all of @p content plus repair packets, so a single photo can recover it.
 * @param content Packed content (see PackContent). Must not be empty.
 * @param sessionId Session id for every packet.
 * @param layout Configured frame layout.
 * @param payload Receives layout.PayloadBytes() bytes on success.
 * @return StillCode::Ok on success, StillCode::TooLarge when the content needs more than one frame.
 */
StillCode BuildStillPayload(std::span<const std::uint8_t> content, std::uint32_t sessionId, const FrameLayout& layout,
                            std::vector<std::uint8_t>& payload);

} // namespace qqr
