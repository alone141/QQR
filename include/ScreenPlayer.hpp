#pragma once

#include "Image.hpp"

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

struct ScreenInfo
{
    int width{};
    int height{};
    int refreshRate{};
};

using PlayerProgress = std::function<void(const PlayerStatus&)>;
using FrameSource = std::function<Image(std::uint64_t frameIndex)>;

/**
 * @brief Measure the area frames will be shown in: the primary monitor, or a window of the configured size.
 * @param config Only windowWidth and windowHeight are used.
 * @param out Receives the area in physical pixels and the monitor refresh rate.
 * @return PlayCode::Ok on success, otherwise why there is no display.
 */
PlayCode QueryScreen(const PlayerConfig& config, ScreenInfo& out);

/**
 * @brief Show frames from @p source until Esc. Starts paused on frame 0; Space starts and pauses.
 * @param source Draws frame n; called in order from a worker thread. Frames should match the QueryScreen size.
 * @param config Only cellPixels, displayFps, windowWidth and windowHeight are used. cellPixels sizes the pass counter.
 * @param status Layout fields are kept as given; a framesPerPass above zero shows "pass N.NN" in the bottom quiet zone.
 *        Screen, timing, framesShown and paused are filled in.
 * @param progress Called about once per second while playing and once after the window opens; may be empty.
 * @return PlayCode::Ok after the user closes the player, otherwise why it could not start.
 */
PlayCode PlayFrames(const FrameSource& source, const PlayerConfig& config, PlayerStatus& status, const PlayerProgress& progress);

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
