#include "VideoSource.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace vfdt
{

namespace
{

#ifdef _WIN32
using CommandString = std::wstring;

CommandString Quote(const std::filesystem::path& path)
{
    return L"\"" + path.wstring() + L"\"";
}

std::FILE* OpenPipe(const CommandString& command)
{
    // cmd.exe /c strips one pair of outer quotes, so wrap the whole line once more.
    const CommandString wrapped = L"\"" + command + L"\"";
    return _wpopen(wrapped.c_str(), L"rb");
}

int ClosePipe(std::FILE* file)
{
    return _pclose(file);
}

std::FILE* OpenFile(const std::filesystem::path& path)
{
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), L"rb") == 0 ? file : nullptr;
}
#else
using CommandString = std::string;

CommandString Quote(const std::filesystem::path& path)
{
    return "'" + path.string() + "'";
}

std::FILE* OpenPipe(const CommandString& command)
{
    return popen(command.c_str(), "r");
}

int ClosePipe(std::FILE* file)
{
    return pclose(file);
}

std::FILE* OpenFile(const std::filesystem::path& path)
{
    return std::fopen(path.c_str(), "rb");
}
#endif

CommandString Widen(std::string_view text)
{
    return CommandString(text.begin(), text.end());
}

bool ParsePositive(std::string_view text, int& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out > 0;
}

// A rate of "0/0" (a still image) or one that does not parse gives 0.
double ParseRate(std::string_view text)
{
    const std::size_t slash = text.find('/');
    int numerator = 0;
    int denominator = 0;
    if (slash == std::string_view::npos || !ParsePositive(text.substr(0, slash), numerator) || !ParsePositive(text.substr(slash + 1), denominator))
    {
        return 0.0;
    }
    return static_cast<double>(numerator) / denominator;
}

bool ProbeFrameSize(const std::filesystem::path& ffprobe, const std::filesystem::path& video, int& width, int& height, double& frameRate)
{
    const CommandString command = Quote(ffprobe) +
                                  Widen(" -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate:format=format_name"
                                        " -of csv=p=0:s=x ") +
                                  Quote(video);
    std::FILE* pipe = OpenPipe(command);
    if (pipe == nullptr)
    {
        return false;
    }
    std::array<char, 128> buffer{};
    std::array<char, 128> format{};
    const bool gotLine = std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr;
    const bool gotFormat = gotLine && std::fgets(format.data(), static_cast<int>(format.size()), pipe) != nullptr;
    ClosePipe(pipe);
    if (!gotLine)
    {
        return false;
    }
    // Still images come through image demuxers (image2, png_pipe, jpeg_pipe, ...) that report a default 25 fps.
    const std::string_view formatName{format.data()};
    const bool still = gotFormat && (formatName.find("image") != std::string_view::npos || formatName.find("_pipe") != std::string_view::npos);

    // Expected output: "3840x2160x60/1" followed by a line break.
    std::string_view line{buffer.data()};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    {
        line.remove_suffix(1);
    }
    const std::size_t first = line.find('x');
    if (first == std::string_view::npos || !ParsePositive(line.substr(0, first), width))
    {
        return false;
    }
    const std::string_view rest = line.substr(first + 1);
    const std::size_t second = rest.find('x');
    frameRate = second == std::string_view::npos || still ? 0.0 : ParseRate(rest.substr(second + 1));
    return ParsePositive(rest.substr(0, second), height);
}

} // namespace

void VideoSource::StreamCloser::operator()(std::FILE* file) const
{
    if (isPipe)
    {
        ClosePipe(file);
    }
    else
    {
        std::fclose(file);
    }
}

VideoSource::VideoSource()
    : stream{nullptr, StreamCloser{}}
    , width{0}
    , height{0}
    , frameRate{0.0}
    , format{PixelFormat::kRgb24}
{
}

VideoCode VideoSource::OpenVideo(const std::filesystem::path& ffmpeg, const std::filesystem::path& ffprobe, const std::filesystem::path& video)
{
    stream.reset();
    width = 0;
    height = 0;
    frameRate = 0.0;
    int probedWidth = 0;
    int probedHeight = 0;
    double probedRate = 0.0;
    if (!ProbeFrameSize(ffprobe, video, probedWidth, probedHeight, probedRate))
    {
        return VideoCode::ProbeFailed;
    }

    // Phone video is stored as YUV 4:2:0, so asking for it back spares FFmpeg a colour conversion that costs more
    // than the decoding itself and halves what goes through the pipe.
    const CommandString command =
        Quote(ffmpeg) + Widen(" -v error -nostdin -noautorotate -i ") + Quote(video) + Widen(" -f rawvideo -pix_fmt yuv420p -");
    std::FILE* pipe = OpenPipe(command);
    if (pipe == nullptr)
    {
        return VideoCode::OpenFailed;
    }
    stream = std::unique_ptr<std::FILE, StreamCloser>{pipe, StreamCloser{true}};
    width = probedWidth;
    height = probedHeight;
    frameRate = probedRate;
    format = PixelFormat::kYuv420;
    return VideoCode::Ok;
}

VideoCode VideoSource::OpenRaw(const std::filesystem::path& file, int frameWidth, int frameHeight)
{
    stream.reset();
    width = 0;
    height = 0;
    frameRate = 0.0;
    std::FILE* opened = OpenFile(file);
    if (opened == nullptr)
    {
        return VideoCode::OpenFailed;
    }
    stream = std::unique_ptr<std::FILE, StreamCloser>{opened, StreamCloser{false}};
    width = frameWidth;
    height = frameHeight;
    format = PixelFormat::kRgb24;
    return VideoCode::Ok;
}

VideoCode VideoSource::ReadFrame(Image& out)
{
    if (!stream)
    {
        return VideoCode::EndOfStream;
    }
    if (out.width != width || out.height != height || out.format != format)
    {
        out = format == PixelFormat::kYuv420 ? MakeYuv420Image(width, height) : MakeImage(width, height);
    }
    const std::size_t read = std::fread(out.pixels.data(), 1, out.pixels.size(), stream.get());
    return read == out.pixels.size() ? VideoCode::Ok : VideoCode::EndOfStream;
}

int VideoSource::Width() const
{
    return width;
}

int VideoSource::Height() const
{
    return height;
}

double VideoSource::FrameRate() const
{
    return frameRate;
}

} // namespace vfdt
