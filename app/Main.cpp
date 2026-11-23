#include "Calibration.hpp"
#include "FrameLayout.hpp"
#include "FrameRenderer.hpp"
#include "Image.hpp"
#include "Manifest.hpp"
#include "Receiver.hpp"
#include "ScreenPlayer.hpp"
#include "Simulation.hpp"
#include "StillEncoder.hpp"
#include "VideoSource.hpp"

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
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
#include <utility>
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

constexpr int recommended_video_side = 3840;
constexpr double recommended_video_fps = 50.0;

// Phones fall back to 1080p 30 fps easily; say so, since small cells then stop reading.
void WarnAboutRecording(const vfdt::VideoSource& source)
{
    if (source.FrameRate() <= 0.0)
    {
        return;
    }
    std::printf("  recorded at %dx%d, %.1f fps\n", source.Width(), source.Height(), source.FrameRate());
    if (std::max(source.Width(), source.Height()) < recommended_video_side)
    {
        std::printf("  warning: below 4K. Small cells need 4K (3840x2160); check the phone's video resolution setting.\n");
    }
    if (source.FrameRate() < recommended_video_fps)
    {
        std::printf("  warning: below 60 fps. Captures per shown frame drop, and motion blur and rolling-shutter skew grow.\n");
    }
}

void PrintUsage()
{
    std::puts(
        "usage:\n"
        "  vfdt play <file> [options]           show <file> as frames on the screen\n"
        "  vfdt image <file> [options]          encode a small <file> into one PNG to photograph\n"
        "  vfdt decode <input>... [options]     recover the file from phone videos or photos\n"
        "  vfdt simulate <file> [options]       send <file> through a simulated camera\n"
        "  vfdt calibrate show [options]        cycle test frames of several cell sizes on the screen\n"
        "  vfdt calibrate check <video>... [o]  measure a recording of them and pick the cell size\n"
        "  vfdt calibrate simulate [options]    run the calibration through a simulated camera\n"
        "\n"
        "play options:\n"
        "  --cell <px>          cell size in screen pixels (6)\n"
        "  --colors <2|4>       colours per cell (4); decode must use the same\n"
        "  --fps <f>            data frames per second (20); rounded to the refresh rate\n"
        "  --repair <ratio>     repair symbols per pass, share of source symbols (0.25)\n"
        "  --window <WxH>       play in a window instead of full screen\n"
        "\n"
        "image options:\n"
        "  --out <png>          output file (<file>.png)\n"
        "  --size <auto|WxH>    screen size the image is made for (auto: smallest of 720p..4K that fits)\n"
        "  --cell <px>          cell size in pixels (6)\n"
        "  --colors <2|4>       colours per cell (4); decode must use the same\n"
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
        "  --record <file>      write every camera capture as raw RGB24 (input for decode --raw)\n"
        "  --full               keep capturing every pass even after the file is recovered\n"
        "\n"
        "calibrate show options:\n"
        "  --cells <list>       cell sizes to try, largest first, up to 8 (12,10,8,6,5,4,3)\n"
        "  --colors <2|4>       colours per cell (4); check must use the same\n"
        "  --seconds <s>        how long each cell size is shown (3)\n"
        "  --fps <f>            frames per second (20); use the rate you will play at\n"
        "  --window <WxH>       show in a window instead of full screen\n"
        "\n"
        "calibrate check options:\n"
        "  --colors, --ffmpeg, --threads, --raw   as for decode\n"
        "\n"
        "calibrate simulate options:\n"
        "  --cells, --colors, --seconds, --fps    as for calibrate show\n"
        "  --screen, --camera, --camera-fps, --blur, --noise, --tilt, --coverage, --seed   as for simulate");
}

bool ParseCellList(std::string_view text, std::vector<int>& cells)
{
    cells.clear();
    while (!text.empty())
    {
        const std::size_t comma = text.find(',');
        int cell = 0;
        if (!ParseInt(text.substr(0, comma), cell) || cell <= 0)
        {
            return false;
        }
        cells.push_back(cell);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return !cells.empty();
}

void ResolveFfmpeg(const char* argv0, std::filesystem::path directory, std::filesystem::path& ffmpeg, std::filesystem::path& ffprobe)
{
    if (directory.empty())
    {
        std::error_code error;
        const std::filesystem::path besideExecutable = std::filesystem::absolute(argv0, error).parent_path();
        if (std::filesystem::exists(besideExecutable / ffmpeg_name, error))
        {
            directory = besideExecutable;
        }
    }
    ffmpeg = directory.empty() ? std::filesystem::path{ffmpeg_name} : directory / ffmpeg_name;
    ffprobe = directory.empty() ? std::filesystem::path{ffprobe_name} : directory / ffprobe_name;
}

const char* CalibrationProblem(vfdt::CalibrationCode code)
{
    switch (code)
    {
    case vfdt::CalibrationCode::Ok:
        return "ok";
    case vfdt::CalibrationCode::BadStepCount:
        return "give between 1 and 8 cell sizes";
    case vfdt::CalibrationCode::BadCellSize:
        return "a cell size is too large for the screen (the grid needs at least 64 cells each way)";
    case vfdt::CalibrationCode::BadTiming:
        return "--seconds and --fps must give between 1 and 65535 frames per cell size";
    case vfdt::CalibrationCode::BadBitsPerCell:
        return "--colors must be 2 or 4";
    }
    return "unknown problem";
}

void PrintCalibrationReport(const vfdt::CalibrationReport& report)
{
    if (!report.found)
    {
        std::printf("no calibration frame was read in %llu images.\n", static_cast<unsigned long long>(report.captures));
        std::printf("check that the whole screen is in view and in focus, that the video shows vfdt calibrate show,\n"
                    "and that --colors matches. If it still fails, try --cells 16,14,12 to find a size that reads at all.\n");
        return;
    }
    std::printf("%llu images, %d colours, %.1f frames/s; %llu images outside the calibration\n\n",
                static_cast<unsigned long long>(report.captures), 1 << report.bitsPerCell, report.displayFps,
                static_cast<unsigned long long>(report.unattributed));
    std::printf(" cell  grid       bytes/frame  images  read  blend  lost   yield  errors/cw avg max  est. MB/min\n");
    for (std::size_t s = 0; s < report.steps.size(); ++s)
    {
        const vfdt::CalibrationStep& step = report.steps[s];
        char grid[32] = "-";
        if (step.gridWidth > 0)
        {
            std::snprintf(grid, sizeof(grid), "%dx%d", step.gridWidth, step.gridHeight);
        }
        const char* verdict = step.reliable ? "ok" : "";
        if (step.captures == 0)
        {
            verdict = "not in the recording";
        }
        else if (step.framesRead == 0)
        {
            verdict = "unreadable";
        }
        std::printf(" %3d   %-10s %11zu  %6llu %5llu  %5llu %5llu  %5.1f%%  %13.1f %3d  %11.1f  %s\n", step.cellPixels, grid,
                    step.payloadBytes, static_cast<unsigned long long>(step.captures), static_cast<unsigned long long>(step.framesRead),
                    static_cast<unsigned long long>(step.blendedOrTorn), static_cast<unsigned long long>(step.notLocated),
                    100.0 * step.frameYield, step.meanCorrectedBytes, step.maxCorrectedBytes,
                    step.bytesPerSecond * 60.0 / (1024.0 * 1024.0), verdict);
    }
    std::printf("\nyield: share of the data shown that was read at least once. errors/cw: bytes Reed-Solomon fixed per\n"
                "codeword (it can fix %d), in the best capture of each frame. A size is ok at %.0f%% yield, average\n"
                "errors at most %.0f and worst at most %d.\n\n",
                vfdt::rs_parity_bytes / 2, 100.0 * vfdt::calibration_target_yield, vfdt::calibration_mean_error_limit,
                vfdt::calibration_worst_error_limit);
    if (report.recommendedStep < 0)
    {
        std::printf("no cell size was reliable. Hold the phone steadier (a tripod helps), fill more of the frame with the\n"
                    "screen, check focus, dim the room lights, or try larger cells or --colors 2.\n");
        return;
    }
    const vfdt::CalibrationStep& best = report.steps[static_cast<std::size_t>(report.recommendedStep)];
    std::printf("recommended: --cell %d (about %.1f MB/min)\n", best.cellPixels, best.bytesPerSecond * 60.0 / (1024.0 * 1024.0));
    std::printf("  vfdt play <file> --cell %d --colors %d --fps %.0f\n", best.cellPixels, 1 << report.bitsPerCell, report.displayFps);
    std::printf("for large transfers leave some margin: the next larger cell size is safer if lighting or distance may change.\n");
}

struct CalibrationOptions
{
    vfdt::CalibrationPlan plan{};
    double seconds{3.0};
    vfdt::PlayerConfig player{};
    vfdt::SimulationConfig simulation{};
};

// Shared parser for calibrate show and calibrate simulate; simulate also takes the camera options.
bool ParseCalibrationOption(std::string_view option, std::string_view value, bool simulate, CalibrationOptions& options, bool& known)
{
    known = true;
    if (option == "--cells")
    {
        return ParseCellList(value, options.plan.cellSizes);
    }
    if (option == "--colors")
    {
        return ParseColors(value, options.plan.bitsPerCell);
    }
    if (option == "--seconds")
    {
        return ParseDouble(value, options.seconds) && options.seconds > 0.0;
    }
    if (option == "--fps")
    {
        return ParseDouble(value, options.plan.displayFps) && options.plan.displayFps > 0.0;
    }
    if (!simulate && option == "--window")
    {
        return ParseSize(value, options.player.windowWidth, options.player.windowHeight);
    }
    if (simulate)
    {
        vfdt::SimulationConfig& config = options.simulation;
        if (option == "--screen")
        {
            return ParseSize(value, options.plan.screenWidth, options.plan.screenHeight);
        }
        if (option == "--camera")
        {
            return ParseSize(value, config.camera.width, config.camera.height);
        }
        if (option == "--camera-fps")
        {
            return ParseDouble(value, config.cameraFps) && config.cameraFps > 0.0;
        }
        if (option == "--blur")
        {
            return ParseDouble(value, config.camera.blurSigma);
        }
        if (option == "--noise")
        {
            return ParseDouble(value, config.camera.noiseSigma);
        }
        if (option == "--tilt")
        {
            return ParseDouble(value, config.camera.tilt);
        }
        if (option == "--coverage")
        {
            return ParseDouble(value, config.camera.coverage);
        }
        if (option == "--seed")
        {
            int seed = 0;
            const bool ok = ParseInt(value, seed);
            config.camera.seed = static_cast<std::uint32_t>(seed);
            return ok;
        }
    }
    known = false;
    return false;
}

int ParseCalibrationOptions(int argc, char** argv, bool simulate, CalibrationOptions& options)
{
    for (int i = 3; i < argc; i += 2)
    {
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "missing value for %s\n", argv[i]);
            return exit_usage;
        }
        bool known = true;
        const bool ok = ParseCalibrationOption(argv[i], argv[i + 1], simulate, options, known);
        if (!known)
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
    return exit_ok;
}

int RunCalibrateShow(int argc, char** argv)
{
    CalibrationOptions options{};
    if (const int parsed = ParseCalibrationOptions(argc, argv, false, options); parsed != exit_ok)
    {
        return parsed;
    }
    options.player.displayFps = options.plan.displayFps;
    vfdt::ScreenInfo screen{};
    const vfdt::PlayCode queried = vfdt::QueryScreen(options.player, screen);
    if (queried == vfdt::PlayCode::Unsupported)
    {
        std::fprintf(stderr, "the player is only available on Windows\n");
        return exit_failed;
    }
    if (queried != vfdt::PlayCode::Ok)
    {
        std::fprintf(stderr, "cannot find the primary display\n");
        return exit_failed;
    }

    // The player rounds the frame rate to whole refreshes; the frames must carry the rate actually shown.
    const int refreshesPerFrame = std::max(1, static_cast<int>(std::lround(screen.refreshRate / options.plan.displayFps)));
    options.plan.displayFps = static_cast<double>(screen.refreshRate) / refreshesPerFrame;
    options.plan.screenWidth = screen.width;
    options.plan.screenHeight = screen.height;
    options.plan.framesPerStep = std::max(1, static_cast<int>(std::lround(options.seconds * options.plan.displayFps)));
    vfdt::CalibrationRenderer renderer{};
    const vfdt::CalibrationCode configured = renderer.Configure(options.plan);
    if (configured != vfdt::CalibrationCode::Ok)
    {
        std::fprintf(stderr, "%s\n", CalibrationProblem(configured));
        return exit_usage;
    }
    const double cycleSeconds = static_cast<double>(renderer.FramesPerCycle()) / options.plan.displayFps;
    std::printf("screen %dx%d at %d Hz, %.1f frames/s, %d colours; cell sizes:", screen.width, screen.height, screen.refreshRate,
                options.plan.displayFps, 1 << options.plan.bitsPerCell);
    for (const int cell : options.plan.cellSizes)
    {
        std::printf(" %d", cell);
    }
    std::printf("\none cycle = %.0f s. Place the phone where it will be for real transfers, start recording at 60 fps,\n"
                "press Space, record one full cycle plus a few seconds, then: vfdt calibrate check <video>\n"
                "Space pauses, Esc quits.\n",
                cycleSeconds);

    const vfdt::FrameSource source = [&renderer](std::uint64_t index)
    {
        return renderer.Render(index);
    };
    vfdt::PlayerStatus status{};
    const vfdt::PlayerProgress progress = [&renderer](const vfdt::PlayerStatus& current)
    {
        const std::uint64_t shown = current.framesShown > 0 ? current.framesShown - 1 : 0;
        const std::uint64_t cycle = shown / std::max<std::uint64_t>(1, renderer.FramesPerCycle());
        std::printf("\r  %s  cell %2d px  cycle %llu      ", current.paused ? "paused " : "playing", renderer.CellPixelsAt(shown),
                    static_cast<unsigned long long>(cycle + 1));
        std::fflush(stdout);
    };
    if (vfdt::PlayFrames(source, options.player, status, progress) != vfdt::PlayCode::Ok)
    {
        std::fprintf(stderr, "\ncannot open a window on the primary display\n");
        return exit_failed;
    }
    std::printf("\nshowed %llu frames\n", static_cast<unsigned long long>(status.framesShown));
    return exit_ok;
}

int RunCalibrateCheck(int argc, char** argv)
{
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path ffmpegDirectory{};
    int bitsPerCell = 2;
    int threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
    int rawWidth = 0;
    int rawHeight = 0;
    for (int i = 3; i < argc; ++i)
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
        if (option == "--colors")
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
    std::filesystem::path ffmpeg{};
    std::filesystem::path ffprobe{};
    ResolveFfmpeg(argv[0], ffmpegDirectory, ffmpeg, ffprobe);

    vfdt::CalibrationAnalyzer analyzer{bitsPerCell, threads};
    std::vector<vfdt::Image> batch(static_cast<std::size_t>(threads));
    std::uint64_t images = 0;
    auto lastProgress = std::chrono::steady_clock::now();
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
        std::printf("measuring %s (%dx%d) with %d threads\n", input.string().c_str(), source.Width(), source.Height(), threads);
        WarnAboutRecording(source);
        bool ended = false;
        while (!ended)
        {
            std::size_t filled = 0;
            while (filled < batch.size() && source.ReadFrame(batch[filled]) == vfdt::VideoCode::Ok)
            {
                ++filled;
            }
            ended = filled < batch.size();
            analyzer.AcceptImages(std::span<const vfdt::Image>{batch.data(), filled});
            images += filled;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastProgress >= std::chrono::seconds{2})
            {
                lastProgress = now;
                std::printf("  %llu images\n", static_cast<unsigned long long>(images));
            }
        }
    }
    const vfdt::CalibrationReport report = analyzer.Report();
    PrintCalibrationReport(report);
    return report.recommendedStep >= 0 ? exit_ok : exit_failed;
}

int RunCalibrateSimulate(int argc, char** argv)
{
    CalibrationOptions options{};
    options.simulation.camera.width = 1920;
    options.simulation.camera.height = 1080;
    if (const int parsed = ParseCalibrationOptions(argc, argv, true, options); parsed != exit_ok)
    {
        return parsed;
    }
    options.plan.framesPerStep = std::max(1, static_cast<int>(std::lround(options.seconds * options.plan.displayFps)));
    vfdt::CalibrationRenderer renderer{};
    const vfdt::CalibrationCode configured = renderer.Configure(options.plan);
    if (configured != vfdt::CalibrationCode::Ok)
    {
        std::fprintf(stderr, "%s\n", CalibrationProblem(configured));
        return exit_usage;
    }
    const vfdt::CameraModel& camera = options.simulation.camera;
    std::printf("simulating screen %dx%d, camera %dx%d at %.0f fps, blur %.1f, noise %.1f\n", options.plan.screenWidth,
                options.plan.screenHeight, camera.width, camera.height, options.simulation.cameraFps, camera.blurSigma, camera.noiseSigma);
    vfdt::CalibrationAnalyzer analyzer{options.plan.bitsPerCell, static_cast<int>(std::max(1U, std::thread::hardware_concurrency()))};
    // Run into the next cycle, as a real recording should, so an unreadable last size sits between readable frames.
    const std::uint64_t frames = renderer.FramesPerCycle() + static_cast<std::uint64_t>(options.plan.framesPerStep);
    vfdt::SimulateCalibration(renderer, camera, options.simulation.cameraFps, options.plan.displayFps, frames, analyzer);
    const vfdt::CalibrationReport report = analyzer.Report();
    PrintCalibrationReport(report);
    return report.recommendedStep >= 0 ? exit_ok : exit_failed;
}

int RunCalibrate(int argc, char** argv)
{
    const std::string_view action = argc >= 3 ? std::string_view{argv[2]} : std::string_view{};
    if (action == "show")
    {
        return RunCalibrateShow(argc, argv);
    }
    if (action == "check")
    {
        return RunCalibrateCheck(argc, argv);
    }
    if (action == "simulate")
    {
        return RunCalibrateSimulate(argc, argv);
    }
    PrintUsage();
    return exit_usage;
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

int RunImage(int argc, char** argv)
{
    if (argc < 3)
    {
        PrintUsage();
        return exit_usage;
    }
    const std::filesystem::path input{argv[2]};
    std::filesystem::path output = input.filename();
    output += ".png";
    std::vector<std::pair<int, int>> sizes{std::pair{1280, 720}, std::pair{1920, 1080}, std::pair{2560, 1440}, std::pair{3840, 2160}};
    int cellPixels = 6;
    int bitsPerCell = 2;
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
        if (option == "--out")
        {
            output = std::filesystem::path{std::string{value}};
        }
        else if (option == "--size")
        {
            int width = 0;
            int height = 0;
            ok = value == "auto" || ParseSize(value, width, height);
            if (ok && value != "auto")
            {
                sizes = {std::pair{width, height}};
            }
        }
        else if (option == "--cell")
        {
            ok = ParseInt(value, cellPixels) && cellPixels > 0;
        }
        else if (option == "--colors")
        {
            ok = ParseColors(value, bitsPerCell);
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
    const std::size_t manifestBytes = content.size() - fileBytes.size();

    vfdt::FrameLayout layout{};
    std::uint64_t capacity = 0;
    bool fits = false;
    std::pair<int, int> chosen{};
    for (const std::pair<int, int>& size : sizes)
    {
        if (layout.Configure(vfdt::GridCells(size.first, cellPixels), vfdt::GridCells(size.second, cellPixels), bitsPerCell) != vfdt::LayoutCode::Ok)
        {
            continue;
        }
        capacity = vfdt::StillCapacity(layout);
        chosen = size;
        if (capacity >= content.size())
        {
            fits = true;
            break;
        }
    }
    if (!fits)
    {
        const std::uint64_t fileCapacity = capacity > manifestBytes ? capacity - manifestBytes : 0;
        std::fprintf(stderr, "%s is %zu bytes; one %dx%d image with %d px cells holds at most %llu bytes of it.\n",
                     input.filename().string().c_str(), fileBytes.size(), chosen.first, chosen.second, cellPixels,
                     static_cast<unsigned long long>(fileCapacity));
        std::fprintf(stderr, "use smaller cells (--cell), a larger --size, or vfdt play for video.\n");
        return exit_failed;
    }

    std::vector<std::uint8_t> payload;
    vfdt::BuildStillPayload(content, vfdt::SessionIdFor(content), layout, payload);
    const vfdt::Image image = vfdt::FrameRenderer{layout, cellPixels}.Render(payload, 0);
    if (vfdt::WritePng(output, image) != vfdt::ImageIoCode::Ok)
    {
        std::fprintf(stderr, "cannot write %s\n", output.string().c_str());
        return exit_failed;
    }
    std::printf("wrote %s: %dx%d pixels for a %dx%d screen, grid %dx%d, %d colours\n", output.string().c_str(), image.width,
                image.height, chosen.first, chosen.second, layout.GridWidth(), layout.GridHeight(), layout.ColorCount());
    std::printf("%zu of %llu bytes used; %d packets, the rest is repair data\n", content.size(), static_cast<unsigned long long>(capacity),
                layout.PacketsPerFrame());
    std::printf("show it at 100%% zoom (one image pixel per screen pixel) or print it, photograph it, then: vfdt decode <photo>\n");
    return exit_ok;
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

    std::filesystem::path ffmpeg{};
    std::filesystem::path ffprobe{};
    ResolveFfmpeg(argv[0], ffmpegDirectory, ffmpeg, ffprobe);

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
        WarnAboutRecording(source);

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
        if (option == "--full")
        {
            config.stopWhenComplete = false;
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
    if (command == "image")
    {
        return RunImage(argc, argv);
    }
    if (command == "decode")
    {
        return RunDecode(argc, argv);
    }
    if (command == "simulate")
    {
        return RunSimulate(argc, argv);
    }
    if (command == "calibrate")
    {
        return RunCalibrate(argc, argv);
    }
    PrintUsage();
    return exit_usage;
}
