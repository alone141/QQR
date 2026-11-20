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

bool ProbeFrameSize(const std::filesystem::path& ffprobe, const std::filesystem::path& video, int& width, int& height)
{
    const CommandString command =
        Quote(ffprobe) + Widen(" -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0:s=x ") + Quote(video);
    std::FILE* pipe = OpenPipe(command);
    if (pipe == nullptr)
    {
        return false;
    }
    std::array<char, 128> buffer{};
    const bool gotLine = std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr;
    ClosePipe(pipe);
    if (!gotLine)
    {
        return false;
    }

    // Expected output: "3840x2160" followed by a line break.
    std::string_view line{buffer.data()};
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
    {
        line.remove_suffix(1);
    }
    const std::size_t split = line.find('x');
    return split != std::string_view::npos && ParsePositive(line.substr(0, split), width) && ParsePositive(line.substr(split + 1), height);
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
{
}

VideoCode VideoSource::OpenVideo(const std::filesystem::path& ffmpeg, const std::filesystem::path& ffprobe, const std::filesystem::path& video)
{
    stream.reset();
    width = 0;
    height = 0;
    int probedWidth = 0;
    int probedHeight = 0;
    if (!ProbeFrameSize(ffprobe, video, probedWidth, probedHeight))
    {
        return VideoCode::ProbeFailed;
    }

    const CommandString command =
        Quote(ffmpeg) + Widen(" -v error -nostdin -noautorotate -i ") + Quote(video) + Widen(" -f rawvideo -pix_fmt rgb24 -");
    std::FILE* pipe = OpenPipe(command);
    if (pipe == nullptr)
    {
        return VideoCode::OpenFailed;
    }
    stream = std::unique_ptr<std::FILE, StreamCloser>{pipe, StreamCloser{true}};
    width = probedWidth;
    height = probedHeight;
    return VideoCode::Ok;
}

VideoCode VideoSource::OpenRaw(const std::filesystem::path& file, int frameWidth, int frameHeight)
{
    stream.reset();
    width = 0;
    height = 0;
    std::FILE* opened = OpenFile(file);
    if (opened == nullptr)
    {
        return VideoCode::OpenFailed;
    }
    stream = std::unique_ptr<std::FILE, StreamCloser>{opened, StreamCloser{false}};
    width = frameWidth;
    height = frameHeight;
    return VideoCode::Ok;
}

VideoCode VideoSource::ReadFrame(Image& out)
{
    if (!stream)
    {
        return VideoCode::EndOfStream;
    }
    if (out.width != width || out.height != height)
    {
        out = MakeImage(width, height);
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

} // namespace vfdt
