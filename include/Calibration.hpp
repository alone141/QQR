#pragma once

#include "CameraSimulator.hpp"
#include "FrameLayout.hpp"
#include "FrameReader.hpp"
#include "FrameRenderer.hpp"
#include "Image.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace qqr
{

constexpr std::size_t max_calibration_steps = 32;
constexpr std::size_t calibration_setting_bytes = 3;
constexpr std::size_t calibration_header_bytes = 17 + max_calibration_steps * calibration_setting_bytes;
constexpr int max_calibration_hold = 255;
constexpr double calibration_target_yield = 0.9;
constexpr double calibration_mean_error_limit = 8.0;
constexpr int calibration_worst_error_limit = 24;
constexpr double calibration_repair_overhead = 1.25;

struct CalibrationPlan
{
    int screenWidth{1920};
    int screenHeight{1080};
    std::vector<int> cellSizes{12, 10, 8, 6, 5, 4, 3};
    std::vector<int> colorBits{2, 1};
    std::vector<double> frameRates{30.0, 20.0};
    double refreshRate{60.0};
    double secondsPerStep{3.0};
};

enum class CalibrationCode
{
    Ok,
    BadStepCount,
    BadCellSize,
    BadTiming,
    BadBitsPerCell,
};

// One combination the calibration tries: a cell size and colour depth, each frame held for hold ticks.
struct CalibrationSetting
{
    int cellPixels{};
    int bitsPerCell{};
    int hold{};

    bool operator==(const CalibrationSetting& other) const = default;
};

struct CalibrationHeader
{
    std::uint32_t frameTick{};
    std::uint16_t ticksPerStep{};
    std::uint16_t tickRateTenths{};
    std::vector<CalibrationSetting> settings;
};

/**
 * @brief Build the payload of one calibration frame: every codeword starts with the header and continues with known filler.
 * @param header Frame tick and plan to embed.
 * @param payloadBytes Payload size of the frame's layout, a multiple of rs_data_bytes.
 * @return The payload.
 */
std::vector<std::uint8_t> MakeCalibrationPayload(const CalibrationHeader& header, std::size_t payloadBytes);

/**
 * @brief Read the header at the start of one codeword's data.
 * @param codewordData The rs_data_bytes data bytes of a corrected codeword.
 * @return The header, or std::nullopt when the codeword does not start with one.
 */
std::optional<CalibrationHeader> ParseCalibrationHeader(std::span<const std::uint8_t> codewordData);

class CalibrationRenderer
{
public:
    /**
     * @brief Create an unconfigured renderer. Call Configure before use.
     */
    CalibrationRenderer();

    /**
     * @brief Check the plan, pick the tick rate, and prepare one layout per cell size and colour depth.
     * @param requested Screen size, cell sizes, colours, frame rates and timing.
     * @return CalibrationCode::Ok on success, otherwise what is wrong with the plan.
     */
    CalibrationCode Configure(const CalibrationPlan& requested);

    /**
     * @brief Return how many ticks the player shows per second.
     * @return The refresh rate divided by the largest whole number every requested frame rate fits.
     */
    double TickRate() const;

    /**
     * @brief Return the ticks in one cycle through every setting.
     * @return Settings().size() * ticks per step.
     */
    std::uint64_t TicksPerCycle() const;

    /**
     * @brief Return the settings in the order they are shown: frame rates, then colours, then cell sizes.
     * @return One entry per step; rates that round to the same hold appear once.
     */
    const std::vector<CalibrationSetting>& Settings() const;

    /**
     * @brief Return the step shown at a tick.
     * @param tick Tick counter from the start of playback.
     * @return Index into Settings().
     */
    std::size_t StepAt(std::uint64_t tick) const;

    /**
     * @brief Return the first tick of the frame shown at a tick.
     * @param tick Tick counter from the start of playback.
     * @return The tick the frame appeared; equal to @p tick when the frame changes there.
     */
    std::uint64_t FrameStartAt(std::uint64_t tick) const;

    /**
     * @brief Draw the calibration frame shown at a tick, centred on a black screen-sized image.
     * @param tick Tick counter from the start of playback; every tick of a held frame gives the same image.
     * @return An image of the plan's screen size.
     */
    Image Render(std::uint64_t tick) const;

private:
    CalibrationPlan plan;
    double tickRate;
    CalibrationHeader timing;
    std::vector<std::size_t> layoutOfStep;
    std::vector<FrameLayout> layouts;
    std::vector<FrameRenderer> renderers;
};

struct CalibrationStep
{
    int cellPixels{};
    int bitsPerCell{};
    double framesPerSecond{};
    int gridWidth{};
    int gridHeight{};
    std::size_t payloadBytes{};
    std::uint64_t captures{};
    std::uint64_t framesRead{};
    std::uint64_t blendedOrTorn{};
    std::uint64_t notLocated{};
    std::uint64_t codewords{};
    std::uint64_t failedCodewords{};
    std::uint64_t wrongCodewords{};
    std::uint64_t correctedBytes{};
    int maxCorrectedBytes{};
    std::uint64_t framesExpected{};
    double frameYield{};
    double meanCorrectedBytes{};
    double bytesPerSecond{};
    bool reliable{};
};

struct CalibrationReport
{
    bool found{};
    double tickRate{};
    std::uint64_t captures{};
    std::uint64_t unattributed{};
    std::vector<CalibrationStep> steps;
    int recommendedStep{-1};
};

class CalibrationAnalyzer
{
public:
    /**
     * @brief Prepare to read a recording of calibration frames of any colour depth.
     * @param threads Images AcceptImages reads at the same time. At least 1.
     */
    explicit CalibrationAnalyzer(int threads);

    /**
     * @brief Read several camera images in parallel and record what each one showed.
     * @param images Camera images in recording order.
     */
    void AcceptImages(std::span<const Image> images);

    /**
     * @brief Summarise every image so far per setting and pick the fastest reliable one.
     * @return The report; found is false when no calibration frame was read.
     */
    CalibrationReport Report() const;

private:
    struct Capture
    {
        ReadCode code{};
        bool identified{};
        std::uint32_t frameTick{};
        int gridWidth{};
        int gridHeight{};
        std::size_t payloadBytes{};
        int codewords{};
        int failedCodewords{};
        int wrongCodewords{};
        int correctedBytes{};
        int maxCorrectedBytes{};
    };

    struct ReadResult
    {
        Capture capture;
        std::optional<CalibrationHeader> header;
    };

    ReadResult ReadImage(std::size_t slot, const Image& image, int firstBits) const;
    void Record(const ReadResult& result);

    // Two readers per thread: index 2 * slot + bitsPerCell - 1.
    std::vector<FrameReader> readers;
    std::vector<Capture> captures;
    std::optional<CalibrationHeader> plan;
    int expectedBits;
};

/**
 * @brief Film calibration frames with a simulated camera and feed every capture to @p analyzer.
 * @param renderer A configured calibration renderer; its tick rate is the display rate.
 * @param camera Camera properties.
 * @param cameraFps Camera frames per second.
 * @param ticks Ticks of calibration to show.
 * @param analyzer Receives the captures.
 */
void SimulateCalibration(const CalibrationRenderer& renderer, const CameraModel& camera, double cameraFps, std::uint64_t ticks,
                         CalibrationAnalyzer& analyzer);

} // namespace qqr
