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
#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
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
void WarnAboutRecording(const qqr::VideoSource& source)
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
        "  qqr play <file> [options]           show <file> as frames on the screen\n"
        "  qqr image <file> [options]          encode a small <file> into one PNG to photograph\n"
        "  qqr decode <input>... [options]     recover the file from phone videos or photos\n"
        "  qqr simulate <file> [options]       send <file> through a simulated camera\n"
        "  qqr calibrate show [options]        cycle test frames of several cell sizes, colours and rates\n"
        "  qqr calibrate check <video>... [o]  measure a recording of them and pick the fastest reliable one\n"
        "  qqr calibrate simulate [options]    run the calibration through a simulated camera\n"
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
        "  --out <dir>          write the recovered file here (qqr-out)\n"
        "  --colors <2|4>       colours per cell (4)\n"
        "  --ffmpeg <dir>       folder holding ffmpeg and ffprobe (next to qqr, else PATH)\n"
        "  --threads <n>        frames read in parallel (all cores)\n"
        "  --raw <WxH>          inputs are raw RGB24 frames of this size, not videos\n"
        "\n"
        "simulate options:\n"
        "  --out <dir>          write the recovered file here (qqr-out)\n"
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
        "calibrate show options (every cell size is tried with every colour count and frame rate, up to 32 in all):\n"
        "  --cells <list>       cell sizes, largest first (12,10,8,6,5,4,3)\n"
        "  --colors <list>      colours per cell, 2 and/or 4 (4,2)\n"
        "  --fps <list>         frame rates, each rounded to whole screen refreshes (30,20)\n"
        "  --seconds <s>        how long each combination is shown (3)\n"
        "  --window <WxH>       show in a window instead of full screen\n"
        "\n"
        "calibrate check options (colours and frame rates are read from the frames):\n"
        "  --ffmpeg, --threads, --raw             as for decode\n"
        "\n"
        "calibrate simulate options:\n"
        "  --cells, --colors, --fps, --seconds    as for calibrate show\n"
        "  --refresh <hz>       simulated screen refresh rate (60)\n"
        "  --screen, --camera, --camera-fps, --blur, --noise, --tilt, --coverage, --seed   as for simulate");
}

template <typename Item, typename Parser>
bool ParseList(std::string_view text, std::vector<Item>& items, Parser parse)
{
    items.clear();
    while (!text.empty())
    {
        const std::size_t comma = text.find(',');
        Item item{};
        if (!parse(text.substr(0, comma), item))
        {
            return false;
        }
        items.push_back(item);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return !items.empty();
}

bool ParsePositiveInt(std::string_view text, int& out)
{
    return ParseInt(text, out) && out > 0;
}

bool ParsePositiveDouble(std::string_view text, double& out)
{
    return ParseDouble(text, out) && out > 0.0;
}

double MegabytesPerMinute(double bytesPerSecond)
{
    return bytesPerSecond * 60.0 / (1024.0 * 1024.0);
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

// Hands every frame of @p source to @p accept in batches of @p batchSize, reading the next batch while the previous
// one is processed. @p betweenBatches runs while nothing is being processed, so it may read what @p accept writes;
// it returns false to stop early.
template <typename Accept, typename BetweenBatches>
void PumpFrames(qqr::VideoSource& source, std::size_t batchSize, Accept accept, BetweenBatches betweenBatches)
{
    std::array<std::vector<qqr::Image>, 2> batches{std::vector<qqr::Image>(batchSize), std::vector<qqr::Image>(batchSize)};
    std::future<void> processing;
    std::size_t reading = 0;
    for (bool ended = false; !ended; reading = 1 - reading)
    {
        std::vector<qqr::Image>& batch = batches[reading];
        std::size_t filled = 0;
        while (filled < batch.size() && source.ReadFrame(batch[filled]) == qqr::VideoCode::Ok)
        {
            ++filled;
        }
        ended = filled < batch.size();
        if (processing.valid())
        {
            processing.get();
        }
        if (!betweenBatches())
        {
            return;
        }
        if (filled > 0)
        {
            processing = std::async(std::launch::async, [&accept, &batch, filled]()
            {
                accept(std::span<const qqr::Image>{batch.data(), filled});
            });
        }
    }
    if (processing.valid())
    {
        processing.get();
    }
}

const char* CalibrationProblem(qqr::CalibrationCode code)
{
    switch (code)
    {
    case qqr::CalibrationCode::Ok:
        return "ok";
    case qqr::CalibrationCode::BadStepCount:
        return "--cells, --colors and --fps give too many combinations; keep their product at 32 or fewer";
    case qqr::CalibrationCode::BadCellSize:
        return "a cell size is too large for the screen (the grid needs at least 64 cells each way)";
    case qqr::CalibrationCode::BadTiming:
        return "--seconds and --fps must show every combination for at least one frame, and each frame for at most 255 refreshes";
    case qqr::CalibrationCode::BadBitsPerCell:
        return "--colors takes 2 and/or 4";
    }
    return "unknown problem";
}

void PrintCalibrationReport(const qqr::CalibrationReport& report)
{
    if (!report.found)
    {
        std::printf("no calibration frame was read in %llu images.\n", static_cast<unsigned long long>(report.captures));
        std::printf("check that the whole screen is in view and in focus and that the video shows qqr calibrate show.\n"
                    "If it still fails, try --cells 16,14,12 to find a size that reads at all.\n");
        return;
    }
    std::printf("%llu images; %llu images outside the calibration\n\n", static_cast<unsigned long long>(report.captures),
                static_cast<unsigned long long>(report.unattributed));
    std::printf(" cell col   fps  grid       bytes/frame  images  read  blend  lost   yield  errors/cw avg max  est. MB/min\n");
    for (std::size_t s = 0; s < report.steps.size(); ++s)
    {
        const qqr::CalibrationStep& step = report.steps[s];
        // A blank line between runs of cell sizes keeps each colour count and frame rate together.
        if (s > 0 && (step.bitsPerCell != report.steps[s - 1].bitsPerCell || step.framesPerSecond != report.steps[s - 1].framesPerSecond))
        {
            std::printf("\n");
        }
        char grid[32] = "-";
        if (step.gridWidth > 0)
        {
            std::snprintf(grid, sizeof(grid), "%dx%d", step.gridWidth, step.gridHeight);
        }
        const char* verdict = step.reliable ? "ok" : "";
        if (static_cast<int>(s) == report.recommendedStep)
        {
            verdict = "ok, fastest";
        }
        if (step.captures == 0)
        {
            verdict = "not in the recording";
        }
        else if (step.framesRead == 0)
        {
            verdict = "unreadable";
        }
        std::printf(" %3d   %d  %4.1f  %-10s %11zu  %6llu %5llu  %5llu %5llu  %5.1f%%  %13.1f %3d  %11.1f  %s\n", step.cellPixels,
                    1 << step.bitsPerCell, step.framesPerSecond, grid, step.payloadBytes, static_cast<unsigned long long>(step.captures),
                    static_cast<unsigned long long>(step.framesRead), static_cast<unsigned long long>(step.blendedOrTorn),
                    static_cast<unsigned long long>(step.notLocated), 100.0 * step.frameYield, step.meanCorrectedBytes,
                    step.maxCorrectedBytes, MegabytesPerMinute(step.bytesPerSecond), verdict);
    }
    std::printf("\nyield: share of the data shown that was read at least once. errors/cw: bytes Reed-Solomon fixed per\n"
                "codeword (it can fix %d), in the best capture of each frame. A combination is ok at %.0f%% yield,\n"
                "average errors at most %.0f and worst at most %d.\n\n",
                qqr::rs_parity_bytes / 2, 100.0 * qqr::calibration_target_yield, qqr::calibration_mean_error_limit,
                qqr::calibration_worst_error_limit);
    if (report.recommendedStep < 0)
    {
        std::printf("nothing was reliable. Hold the phone steadier (a tripod helps), fill more of the frame with the\n"
                    "screen, check focus, dim the room lights, or try larger cells or a lower --fps.\n");
        return;
    }
    const qqr::CalibrationStep& best = report.steps[static_cast<std::size_t>(report.recommendedStep)];
    std::printf("recommended: --cell %d --colors %d --fps %g (about %.1f MB/min)\n", best.cellPixels, 1 << best.bitsPerCell,
                best.framesPerSecond, MegabytesPerMinute(best.bytesPerSecond));
    std::printf("  qqr play <file> --cell %d --colors %d --fps %g\n", best.cellPixels, 1 << best.bitsPerCell, best.framesPerSecond);
    std::printf("for large transfers leave some margin: a larger cell or a lower rate is safer if lighting or distance may change.\n");
}

struct CalibrationOptions
{
    qqr::CalibrationPlan plan{};
    qqr::PlayerConfig player{};
    qqr::SimulationConfig simulation{};
};

// Shared parser for calibrate show and calibrate simulate; simulate also takes the camera options.
bool ParseCalibrationOption(std::string_view option, std::string_view value, bool simulate, CalibrationOptions& options, bool& known)
{
    known = true;
    if (option == "--cells")
    {
        return ParseList(value, options.plan.cellSizes, ParsePositiveInt);
    }
    if (option == "--colors")
    {
        return ParseList(value, options.plan.colorBits, ParseColors);
    }
    if (option == "--fps")
    {
        return ParseList(value, options.plan.frameRates, ParsePositiveDouble);
    }
    if (option == "--seconds")
    {
        return ParsePositiveDouble(value, options.plan.secondsPerStep);
    }
    if (!simulate && option == "--window")
    {
        return ParseSize(value, options.player.windowWidth, options.player.windowHeight);
    }
    if (simulate)
    {
        qqr::SimulationConfig& config = options.simulation;
        if (option == "--refresh")
        {
            return ParsePositiveDouble(value, options.plan.refreshRate);
        }
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
    qqr::ScreenInfo screen{};
    const qqr::PlayCode queried = qqr::QueryScreen(options.player, screen);
    if (queried == qqr::PlayCode::Unsupported)
    {
        std::fprintf(stderr, "the player is only available on Windows\n");
        return exit_failed;
    }
    if (queried != qqr::PlayCode::Ok)
    {
        std::fprintf(stderr, "cannot find the primary display\n");
        return exit_failed;
    }

    // Each frame rate is shown as a whole number of refreshes of this screen.
    options.plan.refreshRate = screen.refreshRate;
    options.plan.screenWidth = screen.width;
    options.plan.screenHeight = screen.height;
    qqr::CalibrationRenderer renderer{};
    const qqr::CalibrationCode configured = renderer.Configure(options.plan);
    if (configured != qqr::CalibrationCode::Ok)
    {
        std::fprintf(stderr, "%s\n", CalibrationProblem(configured));
        return exit_usage;
    }
    options.player.displayFps = renderer.TickRate();
    const std::vector<qqr::CalibrationSetting>& settings = renderer.Settings();
    const double cycleSeconds = static_cast<double>(renderer.TicksPerCycle()) / renderer.TickRate();
    std::printf("screen %dx%d at %d Hz; %zu combinations of cell size, colours and frame rate:\n", screen.width, screen.height,
                screen.refreshRate, settings.size());
    for (std::size_t s = 0; s < settings.size(); ++s)
    {
        const bool newRun = s == 0 || settings[s].bitsPerCell != settings[s - 1].bitsPerCell || settings[s].hold != settings[s - 1].hold;
        if (newRun)
        {
            std::printf("%s  %d colours at %.1f frames/s, cells", s == 0 ? "" : "\n", 1 << settings[s].bitsPerCell,
                        renderer.TickRate() / settings[s].hold);
        }
        std::printf(" %d", settings[s].cellPixels);
    }
    std::printf("\none cycle = %.0f s. Place the phone where it will be for real transfers, start recording in 4K at 60 fps,\n"
                "press Space, record one full cycle plus a few seconds, then: qqr calibrate check <video>\n"
                "Space pauses, Esc quits.\n",
                cycleSeconds);

    // Held frames repeat for several ticks; render each one once.
    std::uint64_t cachedStart = 0;
    qqr::Image cachedFrame{};
    const qqr::FrameSource source = [&renderer, &cachedStart, &cachedFrame](std::uint64_t tick)
    {
        const std::uint64_t start = renderer.FrameStartAt(tick);
        if (cachedFrame.width == 0 || start != cachedStart)
        {
            cachedFrame = renderer.Render(start);
            cachedStart = start;
        }
        return cachedFrame;
    };
    qqr::PlayerStatus status{};
    const qqr::PlayerProgress progress = [&renderer](const qqr::PlayerStatus& current)
    {
        const std::uint64_t shown = current.framesShown > 0 ? current.framesShown - 1 : 0;
        const std::uint64_t cycle = shown / std::max<std::uint64_t>(1, renderer.TicksPerCycle());
        const qqr::CalibrationSetting& setting = renderer.Settings()[renderer.StepAt(shown)];
        std::printf("\r  %s  cell %2d px  %d colours  %4.1f frames/s  cycle %llu      ", current.paused ? "paused " : "playing",
                    setting.cellPixels, 1 << setting.bitsPerCell, renderer.TickRate() / setting.hold, static_cast<unsigned long long>(cycle + 1));
        std::fflush(stdout);
    };
    if (qqr::PlayFrames(source, options.player, status, progress) != qqr::PlayCode::Ok)
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
        if (option == "--ffmpeg")
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

    qqr::CalibrationAnalyzer analyzer{threads};
    std::uint64_t images = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    for (const std::filesystem::path& input : inputs)
    {
        qqr::VideoSource source{};
        const qqr::VideoCode opened = rawWidth > 0 ? source.OpenRaw(input, rawWidth, rawHeight) : source.OpenVideo(ffmpeg, ffprobe, input);
        if (opened == qqr::VideoCode::ProbeFailed)
        {
            std::fprintf(stderr, "cannot read %s with %s: is FFmpeg installed (use --ffmpeg <dir>) and is it a video?\n",
                         input.string().c_str(), ffprobe.string().c_str());
            return exit_failed;
        }
        if (opened != qqr::VideoCode::Ok)
        {
            std::fprintf(stderr, "cannot open %s\n", input.string().c_str());
            return exit_failed;
        }
        std::printf("measuring %s (%dx%d) with %d threads\n", input.string().c_str(), source.Width(), source.Height(), threads);
        WarnAboutRecording(source);
        PumpFrames(
            source, static_cast<std::size_t>(threads),
            [&analyzer, &images](std::span<const qqr::Image> batch)
            {
                analyzer.AcceptImages(batch);
                images += batch.size();
            },
            [&images, &lastProgress]()
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastProgress >= std::chrono::seconds{2})
                {
                    lastProgress = now;
                    std::printf("  %llu images\n", static_cast<unsigned long long>(images));
                }
                return true;
            });
    }
    const qqr::CalibrationReport report = analyzer.Report();
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
    qqr::CalibrationRenderer renderer{};
    const qqr::CalibrationCode configured = renderer.Configure(options.plan);
    if (configured != qqr::CalibrationCode::Ok)
    {
        std::fprintf(stderr, "%s\n", CalibrationProblem(configured));
        return exit_usage;
    }
    const qqr::CameraModel& camera = options.simulation.camera;
    std::printf("simulating screen %dx%d, camera %dx%d at %.0f fps, blur %.1f, noise %.1f\n", options.plan.screenWidth,
                options.plan.screenHeight, camera.width, camera.height, options.simulation.cameraFps, camera.blurSigma, camera.noiseSigma);
    qqr::CalibrationAnalyzer analyzer{static_cast<int>(std::max(1U, std::thread::hardware_concurrency()))};
    // Run into the next cycle, as a real recording should, so an unreadable last step sits between readable frames.
    const std::uint64_t ticks = renderer.TicksPerCycle() + renderer.TicksPerCycle() / renderer.Settings().size();
    qqr::SimulateCalibration(renderer, camera, options.simulation.cameraFps, ticks, analyzer);
    const qqr::CalibrationReport report = analyzer.Report();
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

const char* ResultName(qqr::ReceiveCode code)
{
    switch (code)
    {
    case qqr::ReceiveCode::Ok:
        return "ok";
    case qqr::ReceiveCode::Incomplete:
        return "incomplete";
    case qqr::ReceiveCode::BadManifest:
        return "bad manifest";
    case qqr::ReceiveCode::SizeMismatch:
        return "size mismatch";
    case qqr::ReceiveCode::HashMismatch:
        return "SHA-256 mismatch";
    }
    return "unknown";
}

void PrintReceiverStats(const qqr::ReceiverStats& stats)
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

void PrintSimulationProgress(const qqr::SimulationReport& report)
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
    qqr::PlayerConfig config{};
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
    const std::vector<std::uint8_t> content = qqr::PackContent(input.filename().string(), fileBytes);
    std::printf("playing %s (%zu bytes)\n", input.filename().string().c_str(), fileBytes.size());

    bool printedHeader = false;
    const qqr::PlayerProgress progress = [&printedHeader](const qqr::PlayerStatus& status)
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

    qqr::PlayerStatus status{};
    switch (qqr::PlayContent(content, config, status, progress))
    {
    case qqr::PlayCode::Ok:
        std::printf("\nshowed %llu frames\n", static_cast<unsigned long long>(status.framesShown));
        return exit_ok;
    case qqr::PlayCode::BadLayout:
        std::fprintf(stderr, "the screen is too small for %d px cells\n", config.cellPixels);
        return exit_usage;
    case qqr::PlayCode::NoDisplay:
        std::fprintf(stderr, "cannot open a window on the primary display\n");
        return exit_failed;
    case qqr::PlayCode::Unsupported:
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
    const std::vector<std::uint8_t> content = qqr::PackContent(input.filename().string(), fileBytes);
    const std::size_t manifestBytes = content.size() - fileBytes.size();

    qqr::FrameLayout layout{};
    std::uint64_t capacity = 0;
    bool fits = false;
    std::pair<int, int> chosen{};
    for (const std::pair<int, int>& size : sizes)
    {
        if (layout.Configure(qqr::GridCells(size.first, cellPixels), qqr::GridCells(size.second, cellPixels), bitsPerCell) != qqr::LayoutCode::Ok)
        {
            continue;
        }
        capacity = qqr::StillCapacity(layout);
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
        std::fprintf(stderr, "use smaller cells (--cell), a larger --size, or qqr play for video.\n");
        return exit_failed;
    }

    std::vector<std::uint8_t> payload;
    qqr::BuildStillPayload(content, qqr::SessionIdFor(content), layout, payload);
    const qqr::Image image = qqr::FrameRenderer{layout, cellPixels}.Render(payload, 0);
    if (qqr::WritePng(output, image) != qqr::ImageIoCode::Ok)
    {
        std::fprintf(stderr, "cannot write %s\n", output.string().c_str());
        return exit_failed;
    }
    std::printf("wrote %s: %dx%d pixels for a %dx%d screen, grid %dx%d, %d colours\n", output.string().c_str(), image.width,
                image.height, chosen.first, chosen.second, layout.GridWidth(), layout.GridHeight(), layout.ColorCount());
    std::printf("%zu of %llu bytes used; %d packets, the rest is repair data\n", content.size(), static_cast<unsigned long long>(capacity),
                layout.PacketsPerFrame());
    std::printf("show it at 100%% zoom (one image pixel per screen pixel) or print it, photograph it, then: qqr decode <photo>\n");
    return exit_ok;
}

int RunDecode(int argc, char** argv)
{
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path outDirectory{"qqr-out"};
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

    qqr::Receiver receiver{bitsPerCell, threads};
    const auto started = std::chrono::steady_clock::now();
    auto lastProgress = started;
    for (const std::filesystem::path& input : inputs)
    {
        qqr::VideoSource source{};
        const qqr::VideoCode opened = rawWidth > 0 ? source.OpenRaw(input, rawWidth, rawHeight) : source.OpenVideo(ffmpeg, ffprobe, input);
        if (opened == qqr::VideoCode::ProbeFailed)
        {
            std::fprintf(stderr, "cannot read %s with %s: is FFmpeg installed (use --ffmpeg <dir>) and is it a video?\n",
                         input.string().c_str(), ffprobe.string().c_str());
            return exit_failed;
        }
        if (opened != qqr::VideoCode::Ok)
        {
            std::fprintf(stderr, "cannot open %s\n", input.string().c_str());
            return exit_failed;
        }
        std::printf("decoding %s (%dx%d) with %d threads\n", input.string().c_str(), source.Width(), source.Height(), threads);
        WarnAboutRecording(source);

        PumpFrames(
            source, static_cast<std::size_t>(threads),
            [&receiver](std::span<const qqr::Image> batch)
            {
                receiver.AcceptImages(batch);
            },
            [&receiver, &lastProgress]()
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - lastProgress >= std::chrono::seconds{2})
                {
                    lastProgress = now;
                    const qqr::ReceiverStats& stats = receiver.Stats();
                    std::printf("  %llu images, %llu read, blocks %u/%u\n", static_cast<unsigned long long>(stats.images),
                                static_cast<unsigned long long>(stats.framesRead), receiver.Decoder().CompletedBlocks(),
                                receiver.Decoder().BlockCount());
                }
                return !receiver.Decoder().IsComplete();
            });
        if (receiver.Decoder().IsComplete())
        {
            break;
        }
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const qqr::ReceiverStats& stats = receiver.Stats();
    std::printf("processed in %.1f s (%.1f images/s)\n", seconds, seconds > 0.0 ? static_cast<double>(stats.images) / seconds : 0.0);
    PrintReceiverStats(stats);
    std::printf("blocks %u/%u\n", receiver.Decoder().CompletedBlocks(), receiver.Decoder().BlockCount());

    qqr::UnpackedContent unpacked{};
    const qqr::ReceiveCode result = receiver.Unpack(unpacked);
    std::printf("result: %s\n", ResultName(result));
    if (result == qqr::ReceiveCode::Incomplete)
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
    if (result != qqr::ReceiveCode::Ok)
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
    std::filesystem::path outDirectory{"qqr-out"};
    qqr::SimulationConfig config{};
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
    qqr::SimulationReport report{};
    if (qqr::RunSimulation(input.filename().string(), fileBytes, config, report, PrintSimulationProgress) != qqr::SimulationCode::Ok)
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
    if (report.result != qqr::ReceiveCode::Ok)
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
