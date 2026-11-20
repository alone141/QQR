#include "Simulation.hpp"
#include "FrameLayout.hpp"
#include "FrameRenderer.hpp"
#include "Image.hpp"
#include "Manifest.hpp"
#include "Transmitter.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace vfdt
{

SimulationCode RunSimulation(std::string_view fileName, std::span<const std::uint8_t> fileBytes, const SimulationConfig& config,
                             SimulationReport& report, const SimulationProgress& progress)
{
    FrameLayout layout{};
    const int gridWidth = GridCells(config.screenWidth, config.cellPixels);
    const int gridHeight = GridCells(config.screenHeight, config.cellPixels);
    if (layout.Configure(gridWidth, gridHeight, config.bitsPerCell) != LayoutCode::Ok)
    {
        return SimulationCode::BadLayout;
    }

    const std::vector<std::uint8_t> content = PackContent(fileName, fileBytes);
    const Transmitter transmitter{content, SessionIdFor(content), layout, config.repairRatio};
    const FrameRenderer renderer{layout, config.cellPixels};
    const int screenWidth = (gridWidth + 2 * quiet_zone_cells) * config.cellPixels;
    const int screenHeight = (gridHeight + 2 * quiet_zone_cells) * config.cellPixels;
    CameraSimulator camera{config.camera, screenWidth, screenHeight};
    Receiver receiver{config.bitsPerCell, 1};

    report = SimulationReport{};
    report.gridWidth = gridWidth;
    report.gridHeight = gridHeight;
    report.payloadBytes = layout.PayloadBytes();
    report.packetsPerFrame = layout.PacketsPerFrame();
    report.symbolSize = layout.SymbolSize();
    report.contentBytes = content.size();
    report.framesPerPass = transmitter.FramesPerPass();

    std::mt19937 random{config.camera.seed ^ 0x5EEDU};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    const double phase = unit(random) / config.cameraFps;
    const std::uint64_t lastFrame = transmitter.FramesPerPass() * static_cast<std::uint64_t>(config.maxPasses);

    std::uint64_t shownIndex = 0;
    Image shown = renderer.Render(transmitter.FramePayload(0), 0);
    Image upcoming = renderer.Render(transmitter.FramePayload(1), 1);
    if (!config.dumpDirectory.empty())
    {
        WritePpm(config.dumpDirectory / "screen-0000.ppm", shown);
    }

    std::ofstream recording{};
    if (!config.recordPath.empty())
    {
        recording.open(config.recordPath, std::ios::binary);
    }

    double nextProgress = 1.0;
    for (std::uint64_t k = 0;; ++k)
    {
        const double time = phase + static_cast<double>(k) / config.cameraFps;
        const std::uint64_t frameIndex = static_cast<std::uint64_t>(std::floor(time * config.displayFps));
        if (frameIndex >= lastFrame)
        {
            break;
        }
        while (shownIndex < frameIndex)
        {
            ++shownIndex;
            shown = std::move(upcoming);
            upcoming = renderer.Render(transmitter.FramePayload(shownIndex + 1), static_cast<std::uint32_t>(shownIndex + 1));
        }
        report.contentFrames = frameIndex + 1;
        report.seconds = time;
        if (unit(random) < config.dropRate)
        {
            continue;
        }

        const double switchTime = static_cast<double>(frameIndex + 1) / config.displayFps - time;
        const Image capture = camera.Capture(shown, upcoming, switchTime);
        if (!config.dumpDirectory.empty() && report.cameraFrames == 0)
        {
            WritePpm(config.dumpDirectory / "camera-0000.ppm", capture);
        }
        if (recording.is_open())
        {
            recording.write(reinterpret_cast<const char*>(capture.pixels.data()), static_cast<std::streamsize>(capture.pixels.size()));
        }
        receiver.AcceptImage(capture);
        ++report.cameraFrames;

        report.stats = receiver.Stats();
        report.blockCount = receiver.Decoder().BlockCount();
        report.completedBlocks = receiver.Decoder().CompletedBlocks();
        if (progress && time >= nextProgress)
        {
            progress(report);
            nextProgress += 1.0;
        }
        if (config.stopWhenComplete && receiver.Decoder().IsComplete())
        {
            break;
        }
    }

    report.stats = receiver.Stats();
    report.blockCount = receiver.Decoder().BlockCount();
    report.completedBlocks = receiver.Decoder().CompletedBlocks();
    UnpackedContent unpacked{};
    report.result = receiver.Unpack(unpacked);
    if (report.result == ReceiveCode::Ok)
    {
        report.fileName = unpacked.manifest.fileName;
        report.recoveredFile.assign(unpacked.fileBytes.begin(), unpacked.fileBytes.end());
    }
    return SimulationCode::Ok;
}

} // namespace vfdt
