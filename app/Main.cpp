#include "Simulation.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_failed = 1;
constexpr int exit_usage = 2;

void PrintUsage()
{
    std::puts(
        "usage: vfdt simulate <file> [options]\n"
        "\n"
        "Sends <file> through the full chain with a simulated phone camera and checks it arrives intact.\n"
        "\n"
        "options:\n"
        "  --out <dir>          write the recovered file here (default: vfdt-out)\n"
        "  --screen <WxH>       display resolution (1920x1080)\n"
        "  --cell <px>          cell size in screen pixels (6)\n"
        "  --colors <2|4>       colours per cell (4)\n"
        "  --repair <ratio>     repair symbols per pass, share of source symbols (0.25)\n"
        "  --display-fps <f>    data frames per second (20)\n"
        "  --camera <WxH>       camera resolution (3840x2160)\n"
        "  --camera-fps <f>     camera frames per second (60)\n"
        "  --blur <sigma>       optical blur in camera pixels (1.0)\n"
        "  --noise <sigma>      sensor noise in 8-bit levels (3.0)\n"
        "  --tilt <share>       screen tilt, share of the screen width (0.03)\n"
        "  --coverage <share>   share of the camera width the screen fills (0.85)\n"
        "  --drop <share>       share of camera frames lost (0)\n"
        "  --passes <n>         give up after n passes over the data (3)\n"
        "  --seed <n>           random seed (1)\n"
        "  --dump <dir>         write the first screen frame and camera capture as PPM");
}

bool ParseDouble(std::string_view text, double& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

bool ParseInt(std::string_view text, int& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

bool ParseSize(std::string_view text, int& width, int& height)
{
    const std::size_t split = text.find('x');
    return split != std::string_view::npos && ParseInt(text.substr(0, split), width) && ParseInt(text.substr(split + 1), height) &&
           width > 0 && height > 0;
}

bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
{
    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    return !file.bad();
}

const char* ResultName(vfdt::ReceiveCode code)
{
    switch (code)
    {
    case vfdt::ReceiveCode::Ok:
        return "ok";
    case vfdt::ReceiveCode::Incomplete:
        return "incomplete";
    case vfdt::ReceiveCode::BadManifest:
        return "bad manifest";
    case vfdt::ReceiveCode::SizeMismatch:
        return "size mismatch";
    case vfdt::ReceiveCode::HashMismatch:
        return "SHA-256 mismatch";
    }
    return "unknown";
}

void PrintProgress(const vfdt::SimulationReport& report)
{
    std::printf("  t=%6.1fs  frame %llu/%llu  blocks %u/%u  read %llu/%llu captures\n", report.seconds,
                static_cast<unsigned long long>(report.contentFrames), static_cast<unsigned long long>(report.framesPerPass),
                report.completedBlocks, report.blockCount, static_cast<unsigned long long>(report.stats.framesRead),
                static_cast<unsigned long long>(report.cameraFrames));
}

int RunSimulate(int argc, char** argv)
{
    if (argc < 3)
    {
        PrintUsage();
        return exit_usage;
    }
    const std::filesystem::path input{argv[2]};
    std::filesystem::path outDirectory{"vfdt-out"};
    vfdt::SimulationConfig config{};
    for (int i = 3; i < argc; ++i)
    {
        const std::string_view option{argv[i]};
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            return exit_usage;
        }
        const std::string_view value{argv[++i]};
        int colors = 0;
        bool ok = true;
        if (option == "--out")
        {
            outDirectory = std::filesystem::path{std::string{value}};
        }
        else if (option == "--screen")
        {
            ok = ParseSize(value, config.screenWidth, config.screenHeight);
        }
        else if (option == "--cell")
        {
            ok = ParseInt(value, config.cellPixels) && config.cellPixels > 0;
        }
        else if (option == "--colors")
        {
            ok = ParseInt(value, colors) && (colors == 2 || colors == 4);
            config.bitsPerCell = colors == 2 ? 1 : 2;
        }
        else if (option == "--repair")
        {
            ok = ParseDouble(value, config.repairRatio);
        }
        else if (option == "--display-fps")
        {
            ok = ParseDouble(value, config.displayFps) && config.displayFps > 0.0;
        }
        else if (option == "--camera")
        {
            ok = ParseSize(value, config.camera.width, config.camera.height);
        }
        else if (option == "--camera-fps")
        {
            ok = ParseDouble(value, config.cameraFps) && config.cameraFps > 0.0;
        }
        else if (option == "--blur")
        {
            ok = ParseDouble(value, config.camera.blurSigma);
        }
        else if (option == "--noise")
        {
            ok = ParseDouble(value, config.camera.noiseSigma);
        }
        else if (option == "--tilt")
        {
            ok = ParseDouble(value, config.camera.tilt);
        }
        else if (option == "--coverage")
        {
            ok = ParseDouble(value, config.camera.coverage);
        }
        else if (option == "--drop")
        {
            ok = ParseDouble(value, config.dropRate);
        }
        else if (option == "--passes")
        {
            ok = ParseInt(value, config.maxPasses) && config.maxPasses > 0;
        }
        else if (option == "--seed")
        {
            int seed = 0;
            ok = ParseInt(value, seed);
            config.camera.seed = static_cast<std::uint32_t>(seed);
        }
        else if (option == "--dump")
        {
            config.dumpDirectory = std::filesystem::path{std::string{value}};
        }
        else
        {
            std::fprintf(stderr, "unknown option %s\n", argv[i - 1]);
            return exit_usage;
        }
        if (!ok)
        {
            std::fprintf(stderr, "bad value for %s: %s\n", argv[i - 1], argv[i]);
            return exit_usage;
        }
    }

    std::vector<std::uint8_t> fileBytes;
    if (!ReadFile(input, fileBytes))
    {
        std::fprintf(stderr, "cannot read %s\n", input.string().c_str());
        return exit_failed;
    }
    std::error_code error;
    if (!config.dumpDirectory.empty())
    {
        std::filesystem::create_directories(config.dumpDirectory, error);
    }

    std::printf("sending %s (%zu bytes)\n", input.filename().string().c_str(), fileBytes.size());
    vfdt::SimulationReport report{};
    if (vfdt::RunSimulation(input.filename().string(), fileBytes, config, report, PrintProgress) != vfdt::SimulationCode::Ok)
    {
        std::fprintf(stderr, "screen %dx%d with %d px cells gives no usable grid\n", config.screenWidth, config.screenHeight, config.cellPixels);
        return exit_usage;
    }

    const vfdt::ReceiverStats& stats = report.stats;
    const double megabytes = static_cast<double>(report.contentBytes) / (1024.0 * 1024.0);
    std::printf("grid %dx%d, %d colours, %zu payload bytes/frame, %d packets/frame, %u-byte symbols\n", report.gridWidth,
                report.gridHeight, 1 << config.bitsPerCell, report.payloadBytes, report.packetsPerFrame, report.symbolSize);
    std::printf("raw rate at %.0f frames/s: %.1f MB/min\n", config.displayFps,
                static_cast<double>(report.payloadBytes) * config.displayFps * 60.0 / (1024.0 * 1024.0));
    std::printf("shown %llu frames (one pass = %llu) in %.1f s; effective %.1f MB/min\n",
                static_cast<unsigned long long>(report.contentFrames), static_cast<unsigned long long>(report.framesPerPass),
                report.seconds, report.seconds > 0.0 ? megabytes * 60.0 / report.seconds : 0.0);
    std::printf("captures %llu: read %llu, blended %llu, torn %llu, no finders %llu, bad grid %llu, low contrast %llu\n",
                static_cast<unsigned long long>(report.cameraFrames), static_cast<unsigned long long>(stats.framesRead),
                static_cast<unsigned long long>(stats.blended), static_cast<unsigned long long>(stats.torn),
                static_cast<unsigned long long>(stats.findersNotFound), static_cast<unsigned long long>(stats.unsupportedGrid),
                static_cast<unsigned long long>(stats.lowContrast));
    std::printf("reed-solomon: %llu bytes corrected, %llu codewords lost\n", static_cast<unsigned long long>(stats.correctedBytes),
                static_cast<unsigned long long>(stats.failedCodewords));
    std::printf("packets: %llu used, %llu redundant, %llu damaged; blocks %u/%u\n", static_cast<unsigned long long>(stats.packetsAccepted),
                static_cast<unsigned long long>(stats.packetsRedundant), static_cast<unsigned long long>(stats.packetsDamaged),
                report.completedBlocks, report.blockCount);
    std::printf("result: %s\n", ResultName(report.result));
    if (report.result != vfdt::ReceiveCode::Ok)
    {
        return exit_failed;
    }

    std::filesystem::create_directories(outDirectory, error);
    const std::filesystem::path outPath = outDirectory / std::filesystem::path{report.fileName}.filename();
    std::ofstream out{outPath, std::ios::binary};
    out.write(reinterpret_cast<const char*>(report.recoveredFile.data()), static_cast<std::streamsize>(report.recoveredFile.size()));
    if (!out)
    {
        std::fprintf(stderr, "cannot write %s\n", outPath.string().c_str());
        return exit_failed;
    }
    std::printf("wrote %s (SHA-256 verified)\n", outPath.string().c_str());
    return exit_ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string_view{argv[1]} == "simulate")
    {
        return RunSimulate(argc, argv);
    }
    PrintUsage();
    return exit_usage;
}
