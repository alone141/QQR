#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace vfdt
{

struct PlayerConfig
{
    int cellPixels{6};
    int bitsPerCell{2};
    double displayFps{20.0};
    double repairRatio{0.25};
    int windowWidth{0};
    int windowHeight{0};
};

struct PlayerStatus
{
    int screenWidth{};
    int screenHeight{};
    int gridWidth{};
    int gridHeight{};
    std::size_t payloadBytes{};
    int packetsPerFrame{};
    std::uint64_t framesPerPass{};
    std::uint64_t framesShown{};
    int refreshRate{};
    int refreshesPerFrame{};
    double framesPerSecond{};
    bool paused{};
};

enum class PlayCode
{
    Ok,
    NoDisplay,
    BadLayout,
    Unsupported,
};

using PlayerProgress = std::function<void(const PlayerStatus&)>;

/**
 * @brief Show @p content as frames on the primary monitor until Esc. Starts paused on the first frame; Space starts and pauses.
 * @param content Packed content (see PackContent).
 * @param config Cell size, colours, frame rate, repair ratio; a window size above zero gives a window instead of full screen.
 * @param status Receives the layout and timing; framesShown counts frames shown when the player closes.
 * @param progress Called about once per second while playing and once after the window opens; may be empty.
 * @return PlayCode::Ok after the user closes the player, otherwise why it could not start.
 */
PlayCode PlayContent(std::span<const std::uint8_t> content, const PlayerConfig& config, PlayerStatus& status, const PlayerProgress& progress);

} // namespace vfdt
