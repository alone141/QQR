#pragma once

#include "CameraSimulator.hpp"
#include "Receiver.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vfdt
{

struct SimulationConfig
{
    int screenWidth{1920};
    int screenHeight{1080};
    int cellPixels{6};
    int bitsPerCell{2};
    double repairRatio{0.25};
    double displayFps{20.0};
    double cameraFps{60.0};
    double dropRate{0.0};
    int maxPasses{3};
    CameraModel camera{};
    std::filesystem::path dumpDirectory{};
};

struct SimulationReport
{
    int gridWidth{};
    int gridHeight{};
    std::size_t payloadBytes{};
    int packetsPerFrame{};
    std::uint32_t symbolSize{};
    std::uint64_t contentBytes{};
    std::uint64_t framesPerPass{};
    std::uint64_t contentFrames{};
    std::uint64_t cameraFrames{};
    double seconds{};
    std::uint32_t blockCount{};
    std::uint32_t completedBlocks{};
    ReceiverStats stats{};
    ReceiveCode result{ReceiveCode::Incomplete};
    std::string fileName;
    std::vector<std::uint8_t> recoveredFile;
};

enum class SimulationCode
{
    Ok,
    BadLayout,
};

using SimulationProgress = std::function<void(const SimulationReport&)>;

/**
 * @brief Send a file through the whole chain: packets, frames, simulated phone camera, reader, decoder.
 * @param fileName Name stored in the manifest.
 * @param fileBytes File to send.
 * @param config Screen, density, timing and camera settings.
 * @param report Receives the outcome, the counters and the recovered file.
 * @param progress Called about once per second of simulated time; may be empty.
 * @return SimulationCode::Ok when the simulation ran (check report.result), SimulationCode::BadLayout when the screen settings give no usable grid.
 */
SimulationCode RunSimulation(std::string_view fileName, std::span<const std::uint8_t> fileBytes, const SimulationConfig& config,
                             SimulationReport& report, const SimulationProgress& progress);

} // namespace vfdt
