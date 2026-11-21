#include "Calibration.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace vfdt
{

namespace
{

constexpr std::array<std::uint8_t, 8> calibration_magic{'V', 'F', 'D', 'T', 'C', 'A', 'L', '1'};
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
void FillCodeword(std::uint32_t frameIndex, std::size_t codeword, std::span<std::uint8_t> out)
{
    const std::uint64_t seed = (static_cast<std::uint64_t>(frameIndex) << 32) ^ static_cast<std::uint64_t>(codeword);
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
    return a.framesPerStep == b.framesPerStep && a.fpsTenths == b.fpsTenths && a.bitsPerCell == b.bitsPerCell && a.cellSizes == b.cellSizes;
}

} // namespace

std::vector<std::uint8_t> MakeCalibrationPayload(const CalibrationHeader& header, std::size_t payloadBytes)
{
    std::vector<std::uint8_t> payload(payloadBytes, 0);
    const std::size_t codewords = payloadBytes / rs_data_bytes;
    for (std::size_t c = 0; c < codewords; ++c)
    {
        const std::span<std::uint8_t> data{payload.data() + c * rs_data_bytes, static_cast<std::size_t>(rs_data_bytes)};
        std::copy(calibration_magic.begin(), calibration_magic.end(), data.begin());
        PutU16(data, 8, static_cast<std::uint16_t>(header.frameIndex));
        PutU16(data, 10, static_cast<std::uint16_t>(header.frameIndex >> 16));
        PutU16(data, 12, header.framesPerStep);
        PutU16(data, 14, header.fpsTenths);
        data[16] = header.bitsPerCell;
        data[17] = static_cast<std::uint8_t>(header.cellSizes.size());
        for (std::size_t i = 0; i < header.cellSizes.size() && i < max_calibration_steps; ++i)
        {
            data[18 + i] = static_cast<std::uint8_t>(header.cellSizes[i]);
        }
        FillCodeword(header.frameIndex, c, data.subspan(calibration_header_bytes));
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
    header.frameIndex = static_cast<std::uint32_t>(GetU16(codewordData, 8)) | (static_cast<std::uint32_t>(GetU16(codewordData, 10)) << 16);
    header.framesPerStep = GetU16(codewordData, 12);
    header.fpsTenths = GetU16(codewordData, 14);
    header.bitsPerCell = codewordData[16];
    const std::size_t steps = codewordData[17];
    if (steps == 0 || steps > max_calibration_steps || header.framesPerStep == 0 || header.fpsTenths == 0 ||
        (header.bitsPerCell != 1 && header.bitsPerCell != 2))
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < steps; ++i)
    {
        const int cell = codewordData[18 + i];
        if (cell == 0)
        {
            return std::nullopt;
        }
        header.cellSizes.push_back(cell);
    }
    return header;
}

CalibrationRenderer::CalibrationRenderer()
    : plan{}
    , layouts{}
    , renderers{}
{
}

CalibrationCode CalibrationRenderer::Configure(const CalibrationPlan& requested)
{
    if (requested.cellSizes.empty() || requested.cellSizes.size() > max_calibration_steps)
    {
        return CalibrationCode::BadStepCount;
    }
    if (requested.bitsPerCell != 1 && requested.bitsPerCell != 2)
    {
        return CalibrationCode::BadBitsPerCell;
    }
    if (requested.framesPerStep < 1 || requested.framesPerStep > 0xFFFF || !(requested.displayFps > 0.0) ||
        std::lround(requested.displayFps * 10.0) > 0xFFFF || std::lround(requested.displayFps * 10.0) < 1)
    {
        return CalibrationCode::BadTiming;
    }
    std::vector<FrameLayout> configured;
    std::vector<FrameRenderer> drawn;
    for (const int cell : requested.cellSizes)
    {
        FrameLayout layout{};
        if (cell < 1 || cell > 0xFF ||
            layout.Configure(GridCells(requested.screenWidth, cell), GridCells(requested.screenHeight, cell), requested.bitsPerCell) != LayoutCode::Ok)
        {
            return CalibrationCode::BadCellSize;
        }
        configured.push_back(layout);
        drawn.emplace_back(layout, cell);
    }
    plan = requested;
    layouts = std::move(configured);
    renderers = std::move(drawn);
    return CalibrationCode::Ok;
}

std::uint64_t CalibrationRenderer::FramesPerCycle() const
{
    return static_cast<std::uint64_t>(plan.framesPerStep) * layouts.size();
}

int CalibrationRenderer::CellPixelsAt(std::uint64_t frameIndex) const
{
    const std::size_t step = static_cast<std::size_t>((frameIndex / static_cast<std::uint64_t>(plan.framesPerStep)) % layouts.size());
    return plan.cellSizes[step];
}

Image CalibrationRenderer::Render(std::uint64_t frameIndex) const
{
    const std::size_t step = static_cast<std::size_t>((frameIndex / static_cast<std::uint64_t>(plan.framesPerStep)) % layouts.size());
    CalibrationHeader header{};
    header.frameIndex = static_cast<std::uint32_t>(frameIndex);
    header.framesPerStep = static_cast<std::uint16_t>(plan.framesPerStep);
    header.fpsTenths = static_cast<std::uint16_t>(std::lround(plan.displayFps * 10.0));
    header.bitsPerCell = static_cast<std::uint8_t>(plan.bitsPerCell);
    header.cellSizes = plan.cellSizes;
    const std::vector<std::uint8_t> payload = MakeCalibrationPayload(header, layouts[step].PayloadBytes());
    const Image frame = renderers[step].Render(payload, static_cast<std::uint32_t>(frameIndex));

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

CalibrationAnalyzer::CalibrationAnalyzer(int bitsPerCell, int threads)
    : readers(static_cast<std::size_t>(std::max(1, threads)), FrameReader{bitsPerCell})
    , captures{}
    , plan{}
{
}

void CalibrationAnalyzer::AcceptImages(std::span<const Image> images)
{
    std::vector<FrameRead> frames(readers.size());
    std::vector<std::future<ReadCode>> results;
    for (std::size_t start = 0; start < images.size(); start += readers.size())
    {
        const std::size_t count = std::min(readers.size(), images.size() - start);
        results.clear();
        for (std::size_t i = 0; i < count; ++i)
        {
            results.push_back(std::async(std::launch::async, [this, &images, &frames, start, i]()
            {
                return readers[i].ReadFrame(images[start + i], frames[i]);
            }));
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            Record(results[i].get(), frames[i]);
        }
    }
}

void CalibrationAnalyzer::Record(ReadCode code, const FrameRead& frame)
{
    Capture capture{};
    capture.code = code;
    if (code != ReadCode::Ok)
    {
        captures.push_back(capture);
        return;
    }
    capture.gridWidth = frame.gridWidth;
    capture.gridHeight = frame.gridHeight;
    capture.payloadBytes = frame.payload.size();
    capture.codewords = static_cast<int>(frame.codewordOk.size());
    capture.failedCodewords = frame.failedCodewords;
    capture.correctedBytes = frame.correctedBytes;
    capture.maxCorrectedBytes = frame.maxCorrectedBytes;

    std::optional<CalibrationHeader> header{};
    for (std::size_t c = 0; c < frame.codewordOk.size() && !header.has_value(); ++c)
    {
        if (frame.codewordOk[c])
        {
            header = ParseCalibrationHeader(std::span<const std::uint8_t>{frame.payload}.subspan(c * rs_data_bytes, rs_data_bytes));
        }
    }
    if (header.has_value() && (!plan.has_value() || SamePlan(*plan, *header)))
    {
        if (!plan.has_value())
        {
            plan = header;
        }
        capture.identified = true;
        capture.frameIndex = header->frameIndex;
        const std::vector<std::uint8_t> expected = MakeCalibrationPayload(*header, frame.payload.size());
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
    report.bitsPerCell = plan->bitsPerCell;
    report.displayFps = plan->fpsTenths / 10.0;
    const std::uint64_t framesPerStep = plan->framesPerStep;
    const std::size_t stepCount = plan->cellSizes.size();
    const auto stepOf = [framesPerStep, stepCount](std::uint64_t frameIndex)
    {
        return static_cast<std::size_t>((frameIndex / framesPerStep) % stepCount);
    };
    report.steps.resize(stepCount);
    for (std::size_t s = 0; s < stepCount; ++s)
    {
        report.steps[s].cellPixels = plan->cellSizes[s];
    }

    // A capture that could not be read takes the cell size of the frame shown then, estimated from the
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
    std::uint32_t firstFrame = 0xFFFFFFFFU;
    std::uint32_t lastFrame = 0;
    for (std::size_t i = 0; i < captures.size(); ++i)
    {
        const Capture& capture = captures[i];
        std::optional<std::size_t> step{};
        if (capture.identified)
        {
            step = stepOf(capture.frameIndex);
            firstFrame = std::min(firstFrame, capture.frameIndex);
            lastFrame = std::max(lastFrame, capture.frameIndex);
        }
        else if (previous[i] != no_capture && next[i] != no_capture)
        {
            const Capture& before = captures[static_cast<std::size_t>(previous[i])];
            const Capture& after = captures[static_cast<std::size_t>(next[i])];
            const double share = static_cast<double>(static_cast<std::int64_t>(i) - previous[i]) / static_cast<double>(next[i] - previous[i]);
            const double frame = before.frameIndex + share * (static_cast<double>(after.frameIndex) - before.frameIndex);
            step = stepOf(static_cast<std::uint64_t>(std::max(0.0, std::floor(frame))));
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
            const auto [entry, inserted] = bestCapture.try_emplace(capture.frameIndex, i);
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
    for (const auto& [frame, index] : bestCapture)
    {
        const Capture& capture = captures[index];
        CalibrationStep& target = report.steps[stepOf(frame)];
        target.codewords += static_cast<std::uint64_t>(capture.codewords);
        target.failedCodewords += static_cast<std::uint64_t>(capture.failedCodewords);
        target.wrongCodewords += static_cast<std::uint64_t>(capture.wrongCodewords);
        target.correctedBytes += static_cast<std::uint64_t>(capture.correctedBytes);
        target.maxCorrectedBytes = std::max(target.maxCorrectedBytes, capture.maxCorrectedBytes);
        yieldSum[stepOf(frame)] += GoodShare(capture);
    }
    for (std::uint64_t frame = firstFrame; frame <= lastFrame; ++frame)
    {
        ++report.steps[stepOf(frame)].framesExpected;
    }

    double bestRate = 0.0;
    for (std::size_t s = 0; s < stepCount; ++s)
    {
        CalibrationStep& step = report.steps[s];
        step.frameYield = step.framesExpected > 0 ? yieldSum[s] / static_cast<double>(step.framesExpected) : 0.0;
        const std::uint64_t goodCodewords = step.codewords - step.failedCodewords - step.wrongCodewords;
        step.meanCorrectedBytes = goodCodewords > 0 ? static_cast<double>(step.correctedBytes) / static_cast<double>(goodCodewords) : 0.0;
        step.bytesPerSecond = static_cast<double>(step.payloadBytes) * report.displayFps * step.frameYield / calibration_repair_overhead;
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

void SimulateCalibration(const CalibrationRenderer& renderer, const CameraModel& camera, double cameraFps, double displayFps,
                         std::uint64_t frames, CalibrationAnalyzer& analyzer)
{
    Image shown = renderer.Render(0);
    Image upcoming = renderer.Render(1);
    CameraSimulator simulator{camera, shown.width, shown.height};
    std::mt19937 random{camera.seed ^ 0xCA1U};
    std::uniform_real_distribution<double> unit{0.0, 1.0};
    const double phase = unit(random) / cameraFps;

    std::uint64_t shownIndex = 0;
    std::vector<Image> batch;
    for (std::uint64_t k = 0;; ++k)
    {
        const double time = phase + static_cast<double>(k) / cameraFps;
        const std::uint64_t frameIndex = static_cast<std::uint64_t>(std::floor(time * displayFps));
        if (frameIndex >= frames)
        {
            break;
        }
        while (shownIndex < frameIndex)
        {
            ++shownIndex;
            shown = std::move(upcoming);
            upcoming = renderer.Render(shownIndex + 1);
        }
        const double switchTime = static_cast<double>(frameIndex + 1) / displayFps - time;
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
