#include "Calibration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace vfdt
{

namespace
{

constexpr std::array<std::uint8_t, 8> calibration_magic{'V', 'F', 'D', 'T', 'C', 'A', 'L', '2'};
constexpr std::size_t settings_offset = 17;
constexpr std::size_t simulation_batch_images = 8;
constexpr std::int64_t no_capture = -1;

std::uint64_t Mix(std::uint64_t value)
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

// Filler depends on the frame and codeword, so a codeword shown in the wrong place cannot pass the check.
void FillCodeword(std::uint32_t frameTick, std::size_t codeword, std::span<std::uint8_t> out)
{
    const std::uint64_t seed = (static_cast<std::uint64_t>(frameTick) << 32) ^ static_cast<std::uint64_t>(codeword);
    std::uint64_t word = 0;
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (i % 8 == 0)
        {
            word = Mix(seed * 0x100000001B3ULL + i / 8);
        }
        out[i] = static_cast<std::uint8_t>(word >> (8 * (i % 8)));
    }
}

void PutU16(std::span<std::uint8_t> out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>(value);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

std::uint16_t GetU16(std::span<const std::uint8_t> in, std::size_t offset)
{
    return static_cast<std::uint16_t>(in[offset] | (in[offset + 1] << 8));
}

std::size_t StepOfTick(const CalibrationHeader& timing, std::uint64_t tick)
{
    return static_cast<std::size_t>((tick / timing.ticksPerStep) % timing.settings.size());
}

std::uint64_t FrameStartOfTick(const CalibrationHeader& timing, std::uint64_t tick)
{
    const std::uint64_t hold = static_cast<std::uint64_t>(timing.settings[StepOfTick(timing, tick)].hold);
    return tick - (tick % timing.ticksPerStep) % hold;
}

// Counts frames within a step, so the parity strips alternate between consecutive frames even when frames are held.
std::uint32_t FrameNumberOfTick(const CalibrationHeader& timing, std::uint64_t tick)
{
    const std::uint64_t hold = static_cast<std::uint64_t>(timing.settings[StepOfTick(timing, tick)].hold);
    return static_cast<std::uint32_t>((tick % timing.ticksPerStep) / hold);
}

template <typename CaptureType>
double GoodShare(const CaptureType& capture)
{
    if (capture.codewords <= 0)
    {
        return 0.0;
    }
    return static_cast<double>(capture.codewords - capture.failedCodewords - capture.wrongCodewords) / capture.codewords;
}

template <typename CaptureType>
bool IsBetterCapture(const CaptureType& candidate, const CaptureType& current)
{
    const double candidateShare = GoodShare(candidate);
    const double currentShare = GoodShare(current);
    if (candidateShare != currentShare)
    {
        return candidateShare > currentShare;
    }
    return candidate.correctedBytes < current.correctedBytes;
}

bool SamePlan(const CalibrationHeader& a, const CalibrationHeader& b)
{
    return a.ticksPerStep == b.ticksPerStep && a.tickRateTenths == b.tickRateTenths && a.settings == b.settings;
}

} // namespace

std::vector<std::uint8_t> MakeCalibrationPayload(const CalibrationHeader& header, std::size_t payloadBytes)
{
    std::vector<std::uint8_t> payload(payloadBytes, 0);
    const std::size_t codewords = payloadBytes / rs_data_bytes;
    const std::size_t steps = std::min(header.settings.size(), max_calibration_steps);
    for (std::size_t c = 0; c < codewords; ++c)
    {
        const std::span<std::uint8_t> data{payload.data() + c * rs_data_bytes, static_cast<std::size_t>(rs_data_bytes)};
        std::copy(calibration_magic.begin(), calibration_magic.end(), data.begin());
        PutU16(data, 8, static_cast<std::uint16_t>(header.frameTick));
        PutU16(data, 10, static_cast<std::uint16_t>(header.frameTick >> 16));
        PutU16(data, 12, header.ticksPerStep);
        PutU16(data, 14, header.tickRateTenths);
        data[16] = static_cast<std::uint8_t>(steps);
        for (std::size_t i = 0; i < steps; ++i)
        {
            const CalibrationSetting& setting = header.settings[i];
            const std::size_t at = settings_offset + i * calibration_setting_bytes;
            data[at] = static_cast<std::uint8_t>(setting.cellPixels);
            data[at + 1] = static_cast<std::uint8_t>(setting.bitsPerCell);
            data[at + 2] = static_cast<std::uint8_t>(setting.hold);
        }
        FillCodeword(header.frameTick, c, data.subspan(calibration_header_bytes));
    }
    return payload;
}

std::optional<CalibrationHeader> ParseCalibrationHeader(std::span<const std::uint8_t> codewordData)
{
    if (codewordData.size() < calibration_header_bytes || !std::equal(calibration_magic.begin(), calibration_magic.end(), codewordData.begin()))
    {
        return std::nullopt;
    }
    CalibrationHeader header{};
    header.frameTick = static_cast<std::uint32_t>(GetU16(codewordData, 8)) | (static_cast<std::uint32_t>(GetU16(codewordData, 10)) << 16);
    header.ticksPerStep = GetU16(codewordData, 12);
    header.tickRateTenths = GetU16(codewordData, 14);
    const std::size_t steps = codewordData[16];
    if (steps == 0 || steps > max_calibration_steps || header.ticksPerStep == 0 || header.tickRateTenths == 0)
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < steps; ++i)
    {
        const std::size_t at = settings_offset + i * calibration_setting_bytes;
        const CalibrationSetting setting{codewordData[at], codewordData[at + 1], codewordData[at + 2]};
        if (setting.cellPixels == 0 || (setting.bitsPerCell != 1 && setting.bitsPerCell != 2) || setting.hold == 0)
        {
            return std::nullopt;
        }
        header.settings.push_back(setting);
    }
    return header;
}

CalibrationRenderer::CalibrationRenderer()
    : plan{}
    , tickRate{0.0}
    , timing{}
    , layoutOfStep{}
    , layouts{}
    , renderers{}
{
}

CalibrationCode CalibrationRenderer::Configure(const CalibrationPlan& requested)
{
    if (requested.cellSizes.empty() || requested.colorBits.empty() || requested.frameRates.empty())
    {
        return CalibrationCode::BadStepCount;
    }
    for (const int bits : requested.colorBits)
    {
        if (bits != 1 && bits != 2)
        {
            return CalibrationCode::BadBitsPerCell;
        }
    }
    if (!(requested.refreshRate > 0.0) || !(requested.secondsPerStep > 0.0))
    {
        return CalibrationCode::BadTiming;
    }

    // Every rate is shown as a whole number of refreshes; the tick is the largest step all of them share.
    std::vector<int> refreshesPerFrame;
    for (const double rate : requested.frameRates)
    {
        if (!(rate > 0.0))
        {
            return CalibrationCode::BadTiming;
        }
        const int refreshes = std::max(1, static_cast<int>(std::lround(requested.refreshRate / rate)));
        if (std::find(refreshesPerFrame.begin(), refreshesPerFrame.end(), refreshes) == refreshesPerFrame.end())
        {
            refreshesPerFrame.push_back(refreshes);
        }
    }
    int common = 0;
    for (const int refreshes : refreshesPerFrame)
    {
        common = std::gcd(common, refreshes);
    }
    const double configuredRate = requested.refreshRate / common;
    const long rateTenths = std::lround(configuredRate * 10.0);
    const long ticksPerStep = std::lround(requested.secondsPerStep * configuredRate);
    const int longestHold = *std::max_element(refreshesPerFrame.begin(), refreshesPerFrame.end()) / common;
    if (rateTenths < 1 || rateTenths > 0xFFFF || longestHold > max_calibration_hold || ticksPerStep < longestHold || ticksPerStep > 0xFFFF)
    {
        return CalibrationCode::BadTiming;
    }
    if (refreshesPerFrame.size() * requested.colorBits.size() * requested.cellSizes.size() > max_calibration_steps)
    {
        return CalibrationCode::BadStepCount;
    }

    std::vector<FrameLayout> configured;
    std::vector<FrameRenderer> drawn;
    for (const int bits : requested.colorBits)
    {
        for (const int cell : requested.cellSizes)
        {
            FrameLayout layout{};
            if (cell < 1 || cell > 0xFF ||
                layout.Configure(GridCells(requested.screenWidth, cell), GridCells(requested.screenHeight, cell), bits) != LayoutCode::Ok)
            {
                return CalibrationCode::BadCellSize;
            }
            configured.push_back(layout);
            drawn.emplace_back(layout, cell);
        }
    }

    CalibrationHeader configuredTiming{};
    configuredTiming.ticksPerStep = static_cast<std::uint16_t>(ticksPerStep);
    configuredTiming.tickRateTenths = static_cast<std::uint16_t>(rateTenths);
    std::vector<std::size_t> configuredLayoutOfStep;
    for (const int refreshes : refreshesPerFrame)
    {
        for (std::size_t b = 0; b < requested.colorBits.size(); ++b)
        {
            for (std::size_t c = 0; c < requested.cellSizes.size(); ++c)
            {
                configuredTiming.settings.push_back(CalibrationSetting{requested.cellSizes[c], requested.colorBits[b], refreshes / common});
                configuredLayoutOfStep.push_back(b * requested.cellSizes.size() + c);
            }
        }
    }

    plan = requested;
    tickRate = configuredRate;
    timing = std::move(configuredTiming);
    layoutOfStep = std::move(configuredLayoutOfStep);
    layouts = std::move(configured);
    renderers = std::move(drawn);
    return CalibrationCode::Ok;
}

double CalibrationRenderer::TickRate() const
{
    return tickRate;
}

std::uint64_t CalibrationRenderer::TicksPerCycle() const
{
    return static_cast<std::uint64_t>(timing.ticksPerStep) * timing.settings.size();
}

const std::vector<CalibrationSetting>& CalibrationRenderer::Settings() const
{
    return timing.settings;
}

std::size_t CalibrationRenderer::StepAt(std::uint64_t tick) const
{
    return StepOfTick(timing, tick);
}

std::uint64_t CalibrationRenderer::FrameStartAt(std::uint64_t tick) const
{
    return FrameStartOfTick(timing, tick);
}

Image CalibrationRenderer::Render(std::uint64_t tick) const
{
    const std::size_t step = StepOfTick(timing, tick);
    const std::size_t layout = layoutOfStep[step];
    CalibrationHeader header = timing;
    header.frameTick = static_cast<std::uint32_t>(FrameStartOfTick(timing, tick));
    const std::vector<std::uint8_t> payload = MakeCalibrationPayload(header, layouts[layout].PayloadBytes());
    const Image frame = renderers[layout].Render(payload, FrameNumberOfTick(timing, tick));

    Image canvas = MakeImage(plan.screenWidth, plan.screenHeight);
    const int left = (canvas.width - frame.width) / 2;
    const int top = (canvas.height - frame.height) / 2;
    const std::size_t rowBytes = static_cast<std::size_t>(frame.width) * 3;
    for (int y = 0; y < frame.height; ++y)
    {
        const std::uint8_t* source = frame.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        std::uint8_t* target = canvas.pixels.data() + (static_cast<std::size_t>(top + y) * static_cast<std::size_t>(canvas.width) + static_cast<std::size_t>(left)) * 3;
        std::copy_n(source, rowBytes, target);
    }
    return canvas;
}

CalibrationAnalyzer::CalibrationAnalyzer(int threads)
    : readers{}
    , captures{}
    , plan{}
    , expectedBits{2}
{
    for (int t = 0; t < std::max(1, threads); ++t)
    {
        readers.emplace_back(1);
        readers.emplace_back(2);
    }
}

void CalibrationAnalyzer::AcceptImages(std::span<const Image> images)
{
    const std::size_t slots = readers.size() / 2;
    std::vector<std::future<ReadResult>> results;
    for (std::size_t start = 0; start < images.size(); start += slots)
    {
        const std::size_t count = std::min(slots, images.size() - start);
        const int firstBits = expectedBits;
        results.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            results.push_back(std::async(std::launch::async, [this, &images, start, i, firstBits]()
            {
                return ReadImage(i, images[start + i], firstBits);
            }));
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            Record(results[i].get());
        }
    }
}

CalibrationAnalyzer::ReadResult CalibrationAnalyzer::ReadImage(std::size_t slot, const Image& image, int firstBits) const
{
    const auto readWith = [this, slot, &image](int bits)
    {
        ReadResult result{};
        FrameRead frame{};
        result.capture.code = readers[2 * slot + static_cast<std::size_t>(bits) - 1].ReadFrame(image, frame);
        if (result.capture.code != ReadCode::Ok)
        {
            return result;
        }
        Capture& capture = result.capture;
        capture.gridWidth = frame.gridWidth;
        capture.gridHeight = frame.gridHeight;
        capture.payloadBytes = frame.payload.size();
        capture.codewords = static_cast<int>(frame.codewordOk.size());
        capture.failedCodewords = frame.failedCodewords;
        capture.correctedBytes = frame.correctedBytes;
        capture.maxCorrectedBytes = frame.maxCorrectedBytes;
        for (std::size_t c = 0; c < frame.codewordOk.size() && !result.header.has_value(); ++c)
        {
            if (frame.codewordOk[c])
            {
                result.header = ParseCalibrationHeader(std::span<const std::uint8_t>{frame.payload}.subspan(c * rs_data_bytes, rs_data_bytes));
            }
        }
        if (!result.header.has_value())
        {
            return result;
        }
        const std::vector<std::uint8_t> expected = MakeCalibrationPayload(*result.header, frame.payload.size());
        for (std::size_t c = 0; c < frame.codewordOk.size(); ++c)
        {
            const std::size_t begin = c * rs_data_bytes;
            if (frame.codewordOk[c] && !std::equal(expected.begin() + static_cast<std::ptrdiff_t>(begin),
                                                   expected.begin() + static_cast<std::ptrdiff_t>(begin + rs_data_bytes),
                                                   frame.payload.begin() + static_cast<std::ptrdiff_t>(begin)))
            {
                ++capture.wrongCodewords;
            }
        }
        return result;
    };

    // The colour depth usually matches the last frame identified; only at a step change is the other one needed.
    ReadResult first = readWith(firstBits);
    if (!first.header.has_value())
    {
        ReadResult second = readWith(3 - firstBits);
        if (second.header.has_value())
        {
            return second;
        }
    }
    return first;
}

void CalibrationAnalyzer::Record(const ReadResult& result)
{
    Capture capture = result.capture;
    if (result.header.has_value() && (!plan.has_value() || SamePlan(*plan, *result.header)))
    {
        if (!plan.has_value())
        {
            plan = result.header;
        }
        capture.identified = true;
        capture.frameTick = result.header->frameTick;
        expectedBits = plan->settings[StepOfTick(*plan, capture.frameTick)].bitsPerCell;
    }
    captures.push_back(capture);
}

CalibrationReport CalibrationAnalyzer::Report() const
{
    CalibrationReport report{};
    report.captures = captures.size();
    if (!plan.has_value())
    {
        report.unattributed = captures.size();
        return report;
    }
    report.found = true;
    report.tickRate = plan->tickRateTenths / 10.0;
    const CalibrationHeader& timing = *plan;
    const std::size_t stepCount = timing.settings.size();
    report.steps.resize(stepCount);
    for (std::size_t s = 0; s < stepCount; ++s)
    {
        report.steps[s].cellPixels = timing.settings[s].cellPixels;
        report.steps[s].bitsPerCell = timing.settings[s].bitsPerCell;
        report.steps[s].framesPerSecond = report.tickRate / timing.settings[s].hold;
    }

    // A capture that could not be read takes the setting of the frame shown then, estimated from the
    // identified captures on either side.
    std::vector<std::int64_t> previous(captures.size(), no_capture);
    std::vector<std::int64_t> next(captures.size(), no_capture);
    std::int64_t last = no_capture;
    for (std::size_t i = 0; i < captures.size(); ++i)
    {
        if (captures[i].identified)
        {
            last = static_cast<std::int64_t>(i);
        }
        previous[i] = last;
    }
    last = no_capture;
    for (std::size_t i = captures.size(); i-- > 0;)
    {
        if (captures[i].identified)
        {
            last = static_cast<std::int64_t>(i);
        }
        next[i] = last;
    }

    // Each frame is judged by its best capture. Captures taken while the screen switched frames are damaged in
    // places, and another capture of the same frame usually reads clean; the transfer uses that one too.
    std::map<std::uint32_t, std::size_t> bestCapture;
    std::uint32_t firstTick = 0xFFFFFFFFU;
    std::uint32_t lastTick = 0;
    for (std::size_t i = 0; i < captures.size(); ++i)
    {
        const Capture& capture = captures[i];
        std::optional<std::size_t> step{};
        if (capture.identified)
        {
            step = StepOfTick(timing, capture.frameTick);
            firstTick = std::min(firstTick, capture.frameTick);
            lastTick = std::max(lastTick, capture.frameTick);
        }
        else if (previous[i] != no_capture && next[i] != no_capture)
        {
            const Capture& before = captures[static_cast<std::size_t>(previous[i])];
            const Capture& after = captures[static_cast<std::size_t>(next[i])];
            const double share = static_cast<double>(static_cast<std::int64_t>(i) - previous[i]) / static_cast<double>(next[i] - previous[i]);
            const double tick = before.frameTick + share * (static_cast<double>(after.frameTick) - before.frameTick);
            step = StepOfTick(timing, static_cast<std::uint64_t>(std::max(0.0, std::floor(tick))));
        }
        if (!step.has_value())
        {
            ++report.unattributed;
            continue;
        }

        CalibrationStep& target = report.steps[*step];
        ++target.captures;
        switch (capture.code)
        {
        case ReadCode::Ok:
            // A frame with no readable header was sampled on a wrong grid or too damaged to use: count it as lost.
            if (!capture.identified)
            {
                ++target.notLocated;
                break;
            }
        {
            ++target.framesRead;
            target.gridWidth = capture.gridWidth;
            target.gridHeight = capture.gridHeight;
            target.payloadBytes = capture.payloadBytes;
            const auto [entry, inserted] = bestCapture.try_emplace(capture.frameTick, i);
            if (!inserted && IsBetterCapture(capture, captures[entry->second]))
            {
                entry->second = i;
            }
            break;
        }
        case ReadCode::BlendedFrame:
        case ReadCode::TornFrame:
            ++target.blendedOrTorn;
            break;
        case ReadCode::FindersNotFound:
        case ReadCode::UnsupportedGrid:
        case ReadCode::LowContrast:
            ++target.notLocated;
            break;
        }
    }

    // Yield: of the frames shown while recording, the share of their codewords the best capture read correctly.
    std::vector<double> yieldSum(stepCount, 0.0);
    for (const auto& [frameTick, index] : bestCapture)
    {
        const Capture& capture = captures[index];
        const std::size_t s = StepOfTick(timing, frameTick);
        CalibrationStep& target = report.steps[s];
        target.codewords += static_cast<std::uint64_t>(capture.codewords);
        target.failedCodewords += static_cast<std::uint64_t>(capture.failedCodewords);
        target.wrongCodewords += static_cast<std::uint64_t>(capture.wrongCodewords);
        target.correctedBytes += static_cast<std::uint64_t>(capture.correctedBytes);
        target.maxCorrectedBytes = std::max(target.maxCorrectedBytes, capture.maxCorrectedBytes);
        yieldSum[s] += GoodShare(capture);
    }
    for (std::uint64_t tick = firstTick; tick <= lastTick; ++tick)
    {
        if (FrameStartOfTick(timing, tick) == tick)
        {
            ++report.steps[StepOfTick(timing, tick)].framesExpected;
        }
    }

    double bestRate = 0.0;
    for (std::size_t s = 0; s < stepCount; ++s)
    {
        CalibrationStep& step = report.steps[s];
        step.frameYield = step.framesExpected > 0 ? yieldSum[s] / static_cast<double>(step.framesExpected) : 0.0;
        const std::uint64_t goodCodewords = step.codewords - step.failedCodewords - step.wrongCodewords;
        step.meanCorrectedBytes = goodCodewords > 0 ? static_cast<double>(step.correctedBytes) / static_cast<double>(goodCodewords) : 0.0;
        step.bytesPerSecond = static_cast<double>(step.payloadBytes) * step.framesPerSecond * step.frameYield / calibration_repair_overhead;
        step.reliable = step.framesRead > 0 && step.frameYield >= calibration_target_yield && step.meanCorrectedBytes <= calibration_mean_error_limit &&
                        step.maxCorrectedBytes <= calibration_worst_error_limit && step.wrongCodewords == 0;
        if (step.reliable && step.bytesPerSecond > bestRate)
        {
            bestRate = step.bytesPerSecond;
            report.recommendedStep = static_cast<int>(s);
        }
    }
    return report;
}

void SimulateCalibration(const CalibrationRenderer& renderer, const CameraModel& camera, double cameraFps, std::uint64_t ticks,
                         CalibrationAnalyzer& analyzer)
{
    // Held frames span several ticks; render each frame once and keep it while it can still be on screen.
    std::map<std::uint64_t, Image> frames;
    const auto frameAt = [&renderer, &frames](std::uint64_t tick) -> const Image&
    {
        const std::uint64_t start = renderer.FrameStartAt(tick);
        auto found = frames.find(start);
        if (found == frames.end())
        {
            found = frames.emplace(start, renderer.Render(start)).first;
        }
        return found->second;
    };

    const double tickRate = renderer.TickRate();
    const Image& first = frameAt(0);
    CameraSimulator simulator{camera, first.width, first.height};
    std::mt19937 random{camera.seed ^ 0xCA1U};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    const double phase = unit(random) / cameraFps;

    std::vector<Image> batch;
    for (std::uint64_t k = 0;; ++k)
    {
        const double time = phase + static_cast<double>(k) / cameraFps;
        const std::uint64_t tick = static_cast<std::uint64_t>(std::floor(time * tickRate));
        if (tick >= ticks)
        {
            break;
        }
        frames.erase(frames.begin(), frames.lower_bound(renderer.FrameStartAt(tick)));
        const Image& shown = frameAt(tick);
        const Image& upcoming = frameAt(tick + 1);
        const double switchTime = static_cast<double>(tick + 1) / tickRate - time;
        batch.push_back(simulator.Capture(shown, upcoming, switchTime));
        if (batch.size() == simulation_batch_images)
        {
            analyzer.AcceptImages(batch);
            batch.clear();
        }
    }
    analyzer.AcceptImages(batch);
}

} // namespace vfdt
