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

namespace vfdt
{

constexpr std::size_t calibration_header_bytes = 26;
constexpr std::size_t max_calibration_steps = 8;
constexpr double calibration_target_yield = 0.9;
constexpr double calibration_mean_error_limit = 8.0;
constexpr int calibration_worst_error_limit = 24;
constexpr double calibration_repair_overhead = 1.25;

struct CalibrationPlan
{
    int screenWidth{1920};
    int screenHeight{1080};
    int bitsPerCell{2};
    std::vector<int> cellSizes{12, 10, 8, 6, 5, 4, 3};
    int framesPerStep{60};
    double displayFps{20.0};
};

enum class CalibrationCode
{
    Ok,
    BadStepCount,
    BadCellSize,
    BadTiming,
    BadBitsPerCell,
};

struct CalibrationHeader
{
    std::uint32_t frameIndex{};
    std::uint16_t framesPerStep{};
    std::uint16_t fpsTenths{};
    std::uint8_t bitsPerCell{};
    std::vector<int> cellSizes;
};

/**
 * @brief Build the payload of one calibration frame: every codeword starts with the header and continues with known filler.
 * @param header Frame index and plan to embed.
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
     * @brief Check the plan and prepare one layout per cell size.
     * @param requested Screen size, colours, cell sizes and timing.
     * @return CalibrationCode::Ok on success, otherwise what is wrong with the plan.
     */
    CalibrationCode Configure(const CalibrationPlan& requested);

    /**
     * @brief Return the frames in one cycle through every cell size.
     * @return cellSizes.size() * framesPerStep.
     */
    std::uint64_t FramesPerCycle() const;

    /**
     * @brief Return the cell size shown in a frame.
     * @param frameIndex Frame counter from the start of playback.
     * @return Cell size in screen pixels.
     */
    int CellPixelsAt(std::uint64_t frameIndex) const;

    /**
     * @brief Draw one calibration frame, centred on a black screen-sized image.
     * @param frameIndex Frame counter from the start of playback; the sequence repeats every FramesPerCycle frames.
     * @return An image of the plan's screen size.
     */
    Image Render(std::uint64_t frameIndex) const;

private:
    CalibrationPlan plan;
    std::vector<FrameLayout> layouts;
    std::vector<FrameRenderer> renderers;
};

struct CalibrationStep
{
    int cellPixels{};
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
    int bitsPerCell{};
    double displayFps{};
    std::uint64_t captures{};
    std::uint64_t unattributed{};
    std::vector<CalibrationStep> steps;
    int recommendedStep{-1};
};

class CalibrationAnalyzer
{
public:
    /**
     * @brief Prepare to read a recording of calibration frames.
     * @param bitsPerCell 1 or 2; must match the calibration that was shown.
     * @param threads Images AcceptImages reads at the same time. At least 1.
     */
    CalibrationAnalyzer(int bitsPerCell, int threads);

    /**
     * @brief Read several camera images in parallel and record what each one showed.
     * @param images Camera images in recording order.
     */
    void AcceptImages(std::span<const Image> images);

    /**
     * @brief Summarise every image so far per cell size and pick the fastest reliable one.
     * @return The report; found is false when no calibration frame was read.
     */
    CalibrationReport Report() const;

private:
    struct Capture
    {
        ReadCode code{};
        bool identified{};
        std::uint32_t frameIndex{};
        int gridWidth{};
        int gridHeight{};
        std::size_t payloadBytes{};
        int codewords{};
        int failedCodewords{};
        int wrongCodewords{};
        int correctedBytes{};
        int maxCorrectedBytes{};
    };

    void Record(ReadCode code, const FrameRead& frame);

    std::vector<FrameReader> readers;
    std::vector<Capture> captures;
    std::optional<CalibrationHeader> plan;
};

/**
 * @brief Film calibration frames with a simulated camera and feed every capture to @p analyzer.
 * @param renderer A configured calibration renderer.
 * @param camera Camera properties.
 * @param cameraFps Camera frames per second.
 * @param displayFps Calibration frames per second.
 * @param frames Calibration frames to show.
 * @param analyzer Receives the captures.
 */
void SimulateCalibration(const CalibrationRenderer& renderer, const CameraModel& camera, double cameraFps, double displayFps,
                         std::uint64_t frames, CalibrationAnalyzer& analyzer);

} // namespace vfdt
