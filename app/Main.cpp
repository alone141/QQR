#include "Manifest.hpp"
#include "Receiver.hpp"
#include "ScreenPlayer.hpp"
#include "Simulation.hpp"
#include "VideoSource.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_failed = 1;
constexpr int exit_usage = 2;
constexpr std::size_t max_missing_listed = 20;

#ifdef _WIN32
constexpr std::string_view ffmpeg_name = "ffmpeg.exe";
constexpr std::string_view ffprobe_name = "ffprobe.exe";
#else
constexpr std::string_view ffmpeg_name = "ffmpeg";
constexpr std::string_view ffprobe_name = "ffprobe";
#endif

void PrintUsage()
{
    std::puts(
        "usage:\n"
        "  vfdt play <file> [options]           show <file> as frames on the screen\n"
        "  vfdt decode <video>... [options]     recover the file from phone recordings\n"
        "  vfdt simulate <file> [options]       send <file> through a simulated camera\n"
        "\n"
        "play options:\n"
        "  --cell <px>          cell size in screen pixels (6)\n"
        "  --colors <2|4>       colours per cell (4); decode must use the same\n"
        "  --fps <f>            data frames per second (20); rounded to the refresh rate\n"
        "  --repair <ratio>     repair symbols per pass, share of source symbols (0.25)\n"
        "  --window <WxH>       play in a window instead of full screen\n"
        "\n"
        "decode options:\n"
        "  --out <dir>          write the recovered file here (vfdt-out)\n"
        "  --colors <2|4>       colours per cell (4)\n"
        "  --ffmpeg <dir>       folder holding ffmpeg and ffprobe (next to vfdt, else PATH)\n"
        "  --threads <n>        frames read in parallel (all cores)\n"
        "  --raw <WxH>          inputs are raw RGB24 frames of this size, not videos\n"
        "\n"
        "simulate options:\n"
        "  --out <dir>          write the recovered file here (vfdt-out)\n"
        "  --screen <WxH>       display resolution (1920x1080)\n"
        "  --cell <px>          cell size in screen pixels (6)\n"
        "  --colors <2|4>       colours per cell (4)\n"
        "  --repair <ratio>     repair symbols per pass (0.25)\n"
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
        "  --dump <dir>         write the first screen frame and camera capture as PPM\n"
        "  --record <file>      write every camera capture as raw RGB24 (input for decode --raw)");
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

bool ParseColors(std::string_view text, int& bitsPerCell)
{
    int colors = 0;
    if (!ParseInt(text, colors) || (colors != 2 && colors != 4))
    {
        return false;
    }
    bitsPerCell = colors == 2 ? 1 : 2;
    return true;
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

bool WriteRecoveredFile(const std::filesystem::path& outDirectory, const std::string& fileName, std::span<const std::uint8_t> bytes,
                        std::filesystem::path& written)
{
    std::error_code error;
    std::filesystem::create_directories(outDirectory, error);
    // Only the last path component of the received name is used, so a crafted name cannot escape the folder.
    std::filesystem::path name = std::filesystem::path{fileName}.filename();
    if (name.empty() || name == "." || name == "..")
    {
        name = "received.bin";
    }
    written = outDirectory / name;
    std::ofstream out{written, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
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

void PrintReceiverStats(const vfdt::ReceiverStats& stats)
{
    std::printf("images %llu: read %llu, blended %llu, torn %llu, no finders %llu, bad grid %llu, low contrast %llu\n",
                static_cast<unsigned long long>(stats.images), static_cast<unsigned long long>(stats.framesRead),
                static_cast<unsigned long long>(stats.blended), static_cast<unsigned long long>(stats.torn),
                static_cast<unsigned long long>(stats.findersNotFound), static_cast<unsigned long long>(stats.unsupportedGrid),
                static_cast<unsigned long long>(stats.lowContrast));
    std::printf("reed-solomon: %llu bytes corrected, %llu codewords lost\n", static_cast<unsigned long long>(stats.correctedBytes),
                static_cast<unsigned long long>(stats.failedCodewords));
    std::printf("packets: %llu used, %llu redundant, %llu damaged\n", static_cast<unsigned long long>(stats.packetsAccepted),
                static_cast<unsigned long long>(stats.packetsRedundant), static_cast<unsigned long long>(stats.packetsDamaged));
}

void PrintSimulationProgress(const vfdt::SimulationReport& report)
{
    std::printf("  t=%6.1fs  frame %llu/%llu  blocks %u/%u  read %llu/%llu captures\n", report.seconds,
                static_cast<unsigned long long>(report.contentFrames), static_cast<unsigned long long>(report.framesPerPass),
                report.completedBlocks, report.blockCount, static_cast<unsigned long long>(report.stats.framesRead),
                static_cast<unsigned long long>(report.cameraFrames));
}

int RunPlay(int argc, char** argv)
{
    if (argc < 3)
    {
        PrintUsage();
        return exit_usage;
    }
    const std::filesystem::path input{argv[2]};
    vfdt::PlayerConfig config{};
    for (int i = 3; i < argc; i += 2)
    {
        const std::string_view option{argv[i]};
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            return exit_usage;
        }
        const std::string_view value{argv[i + 1]};
        bool ok = true;
        if (option == "--cell")
        {
            ok = ParseInt(value, config.cellPixels) && config.cellPixels > 0;
        }
        else if (option == "--colors")
        {
            ok = ParseColors(value, config.bitsPerCell);
        }
        else if (option == "--fps")
        {
            ok = ParseDouble(value, config.displayFps) && config.displayFps > 0.0;
        }
        else if (option == "--repair")
        {
            ok = ParseDouble(value, config.repairRatio) && config.repairRatio >= 0.0;
        }
        else if (option == "--window")
        {
            ok = ParseSize(value, config.windowWidth, config.windowHeight);
        }
        else
        {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return exit_usage;
        }
        if (!ok)
        {
            std::fprintf(stderr, "bad value for %s: %s\n", argv[i], argv[i + 1]);
            return exit_usage;
        }
    }

    std::vector<std::uint8_t> fileBytes;
    if (!ReadFile(input, fileBytes))
    {
        std::fprintf(stderr, "cannot read %s\n", input.string().c_str());
        return exit_failed;
    }
    const std::vector<std::uint8_t> content = vfdt::PackContent(input.filename().string(), fileBytes);
    std::printf("playing %s (%zu bytes)\n", input.filename().string().c_str(), fileBytes.size());

    bool printedHeader = false;
    const vfdt::PlayerProgress progress = [&printedHeader](const vfdt::PlayerStatus& status)
    {
        if (!printedHeader)
        {
            printedHeader = true;
            const double passSeconds = static_cast<double>(status.framesPerPass) / status.framesPerSecond;
            std::printf("screen %dx%d at %d Hz: grid %dx%d, %zu bytes/frame, %.1f frames/s (%d refreshes each)\n", status.screenWidth,
                        status.screenHeight, status.refreshRate, status.gridWidth, status.gridHeight, status.payloadBytes,
                        status.framesPerSecond, status.refreshesPerFrame);
            std::printf("one pass = %llu frames = %.0f s. Start recording, then press Space. Record one pass plus ~10 s.\n",
                        static_cast<unsigned long long>(status.framesPerPass), passSeconds);
            std::printf("Space pauses, Esc quits. Playback loops with fresh repair data until Esc.\n");
        }
        const double pass = static_cast<double>(status.framesShown) / static_cast<double>(std::max<std::uint64_t>(1, status.framesPerPass));
        std::printf("\r  %s frame %llu  (pass %.2f)      ", status.paused ? "paused " : "playing", static_cast<unsigned long long>(status.framesShown), pass);
        std::fflush(stdout);
    };

    vfdt::PlayerStatus status{};
    switch (vfdt::PlayContent(content, config, status, progress))
    {
    case vfdt::PlayCode::Ok:
        std::printf("\nshowed %llu frames\n", static_cast<unsigned long long>(status.framesShown));
        return exit_ok;
    case vfdt::PlayCode::BadLayout:
        std::fprintf(stderr, "the screen is too small for %d px cells\n", config.cellPixels);
        return exit_usage;
    case vfdt::PlayCode::NoDisplay:
        std::fprintf(stderr, "cannot open a window on the primary display\n");
        return exit_failed;
    case vfdt::PlayCode::Unsupported:
        std::fprintf(stderr, "the player is only available on Windows\n");
        return exit_failed;
    }
    return exit_failed;
}

int RunDecode(int argc, char** argv)
{
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path outDirectory{"vfdt-out"};
    std::filesystem::path ffmpegDirectory{};
    int bitsPerCell = 2;
    int threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
    int rawWidth = 0;
    int rawHeight = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view option{argv[i]};
        if (!option.starts_with("--"))
        {
            inputs.emplace_back(argv[i]);
            continue;
        }
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            return exit_usage;
        }
        const std::string_view value{argv[++i]};
        bool ok = true;
        if (option == "--out")
        {
            outDirectory = std::filesystem::path{std::string{value}};
        }
        else if (option == "--colors")
        {
            ok = ParseColors(value, bitsPerCell);
        }
        else if (option == "--ffmpeg")
        {
            ffmpegDirectory = std::filesystem::path{std::string{value}};
        }
        else if (option == "--threads")
        {
            ok = ParseInt(value, threads) && threads > 0;
        }
        else if (option == "--raw")
        {
            ok = ParseSize(value, rawWidth, rawHeight);
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
    if (inputs.empty())
    {
        PrintUsage();
        return exit_usage;
    }

    if (ffmpegDirectory.empty())
    {
        std::error_code error;
        const std::filesystem::path besideExecutable = std::filesystem::absolute(argv[0], error).parent_path();
        if (std::filesystem::exists(besideExecutable / ffmpeg_name, error))
        {
            ffmpegDirectory = besideExecutable;
        }
    }
    const std::filesystem::path ffmpeg = ffmpegDirectory.empty() ? std::filesystem::path{ffmpeg_name} : ffmpegDirectory / ffmpeg_name;
    const std::filesystem::path ffprobe = ffmpegDirectory.empty() ? std::filesystem::path{ffprobe_name} : ffmpegDirectory / ffprobe_name;

    vfdt::Receiver receiver{bitsPerCell, threads};
    std::vector<vfdt::Image> batch(static_cast<std::size_t>(threads));
    const auto started = std::chrono::steady_clock::now();
    auto lastProgress = started;
    for (const std::filesystem::path& input : inputs)
    {
        vfdt::VideoSource source{};
        const vfdt::VideoCode opened = rawWidth > 0 ? source.OpenRaw(input, rawWidth, rawHeight) : source.OpenVideo(ffmpeg, ffprobe, input);
        if (opened == vfdt::VideoCode::ProbeFailed)
        {
            std::fprintf(stderr, "cannot read %s with %s: is FFmpeg installed (use --ffmpeg <dir>) and is it a video?\n",
                         input.string().c_str(), ffprobe.string().c_str());
            return exit_failed;
        }
        if (opened != vfdt::VideoCode::Ok)
        {
            std::fprintf(stderr, "cannot open %s\n", input.string().c_str());
            return exit_failed;
        }
        std::printf("decoding %s (%dx%d) with %d threads\n", input.string().c_str(), source.Width(), source.Height(), threads);

        bool ended = false;
        while (!ended && !receiver.Decoder().IsComplete())
        {
            std::size_t filled = 0;
            while (filled < batch.size() && source.ReadFrame(batch[filled]) == vfdt::VideoCode::Ok)
            {
                ++filled;
            }
            ended = filled < batch.size();
            receiver.AcceptImages(std::span<const vfdt::Image>{batch.data(), filled});

            const auto now = std::chrono::steady_clock::now();
            if (now - lastProgress >= std::chrono::seconds{2})
            {
                lastProgress = now;
                const vfdt::ReceiverStats& stats = receiver.Stats();
                std::printf("  %llu images, %llu read, blocks %u/%u\n", static_cast<unsigned long long>(stats.images),
                            static_cast<unsigned long long>(stats.framesRead), receiver.Decoder().CompletedBlocks(),
                            receiver.Decoder().BlockCount());
            }
        }
        if (receiver.Decoder().IsComplete())
        {
            break;
        }
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const vfdt::ReceiverStats& stats = receiver.Stats();
    std::printf("processed in %.1f s (%.1f images/s)\n", seconds, seconds > 0.0 ? static_cast<double>(stats.images) / seconds : 0.0);
    PrintReceiverStats(stats);
    std::printf("blocks %u/%u\n", receiver.Decoder().CompletedBlocks(), receiver.Decoder().BlockCount());

    vfdt::UnpackedContent unpacked{};
    const vfdt::ReceiveCode result = receiver.Unpack(unpacked);
    std::printf("result: %s\n", ResultName(result));
    if (result == vfdt::ReceiveCode::Incomplete)
    {
        if (!receiver.Decoder().HasSession())
        {
            std::printf("no packets were read: check the frame is fully in view, in focus, and --colors matches play\n");
            return exit_failed;
        }
        const std::vector<std::uint32_t> missing = receiver.Decoder().MissingBlocks();
        std::printf("missing %zu blocks:", missing.size());
        for (std::size_t i = 0; i < std::min(missing.size(), max_missing_listed); ++i)
        {
            std::printf(" %u", missing[i]);
        }
        std::printf("%s\nrecord a longer pass and decode it together with these recordings\n", missing.size() > max_missing_listed ? " ..." : "");
        return exit_failed;
    }
    if (result != vfdt::ReceiveCode::Ok)
    {
        return exit_failed;
    }

    std::filesystem::path written{};
    if (!WriteRecoveredFile(outDirectory, unpacked.manifest.fileName, unpacked.fileBytes, written))
    {
        std::fprintf(stderr, "cannot write %s\n", written.string().c_str());
        return exit_failed;
    }
    std::printf("wrote %s (%zu bytes, SHA-256 verified)\n", written.string().c_str(), unpacked.fileBytes.size());
    return exit_ok;
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
            ok = ParseColors(value, config.bitsPerCell);
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
        else if (option == "--record")
        {
            config.recordPath = std::filesystem::path{std::string{value}};
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
    if (vfdt::RunSimulation(input.filename().string(), fileBytes, config, report, PrintSimulationProgress) != vfdt::SimulationCode::Ok)
    {
        std::fprintf(stderr, "screen %dx%d with %d px cells gives no usable grid\n", config.screenWidth, config.screenHeight, config.cellPixels);
        return exit_usage;
    }

    const double megabytes = static_cast<double>(report.contentBytes) / (1024.0 * 1024.0);
    std::printf("grid %dx%d, %d colours, %zu payload bytes/frame, %d packets/frame, %u-byte symbols\n", report.gridWidth,
                report.gridHeight, 1 << config.bitsPerCell, report.payloadBytes, report.packetsPerFrame, report.symbolSize);
    std::printf("raw rate at %.0f frames/s: %.1f MB/min\n", config.displayFps,
                static_cast<double>(report.payloadBytes) * config.displayFps * 60.0 / (1024.0 * 1024.0));
    std::printf("shown %llu frames (one pass = %llu) in %.1f s; effective %.1f MB/min\n",
                static_cast<unsigned long long>(report.contentFrames), static_cast<unsigned long long>(report.framesPerPass),
                report.seconds, report.seconds > 0.0 ? megabytes * 60.0 / report.seconds : 0.0);
    PrintReceiverStats(report.stats);
    std::printf("blocks %u/%u\n", report.completedBlocks, report.blockCount);
    if (!config.recordPath.empty())
    {
        std::printf("recorded %llu captures of %dx%d to %s\n", static_cast<unsigned long long>(report.cameraFrames), config.camera.width,
                    config.camera.height, config.recordPath.string().c_str());
    }
    std::printf("result: %s\n", ResultName(report.result));
    if (report.result != vfdt::ReceiveCode::Ok)
    {
        return exit_failed;
    }

    std::filesystem::path written{};
    if (!WriteRecoveredFile(outDirectory, report.fileName, report.recoveredFile, written))
    {
        std::fprintf(stderr, "cannot write %s\n", written.string().c_str());
        return exit_failed;
    }
    std::printf("wrote %s (SHA-256 verified)\n", written.string().c_str());
    return exit_ok;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string_view command = argc >= 2 ? std::string_view{argv[1]} : std::string_view{};
    if (command == "play")
    {
        return RunPlay(argc, argv);
    }
    if (command == "decode")
    {
        return RunDecode(argc, argv);
    }
    if (command == "simulate")
    {
        return RunSimulate(argc, argv);
    }
    PrintUsage();
    return exit_usage;
}
