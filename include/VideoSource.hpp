#pragma once

#include "Image.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>

namespace qqr
{

enum class VideoCode
{
    Ok,
    ProbeFailed,
    OpenFailed,
    EndOfStream,
};

class VideoSource
{
public:
    /**
     * @brief Create a source with nothing open.
     */
    VideoSource();

    /**
     * @brief Open a video file through FFmpeg. Frames come out as planar YUV 4:2:0 in stored orientation; rotation
     *        tags are ignored.
     * @param ffmpeg The ffmpeg executable, as a path or a bare name looked up on PATH.
     * @param ffprobe The ffprobe executable, as a path or a bare name looked up on PATH.
     * @param video Video file to read.
     * @return VideoCode::Ok on success; ProbeFailed when ffprobe gave no frame size (missing tool or unreadable video).
     */
    VideoCode OpenVideo(const std::filesystem::path& ffmpeg, const std::filesystem::path& ffprobe, const std::filesystem::path& video);

    /**
     * @brief Open a file of raw RGB24 frames written back to back.
     * @param file File to read.
     * @param frameWidth Width of every frame.
     * @param frameHeight Height of every frame.
     * @return VideoCode::Ok on success, VideoCode::OpenFailed when the file cannot be opened.
     */
    VideoCode OpenRaw(const std::filesystem::path& file, int frameWidth, int frameHeight);

    /**
     * @brief Read the next frame.
     * @param out Receives the frame: RGB from a raw file, planar YUV 4:2:0 from a video. Its buffer is reused when it
     *        already has the right size and format.
     * @return VideoCode::Ok on success, VideoCode::EndOfStream when no whole frame is left.
     */
    VideoCode ReadFrame(Image& out);

    /**
     * @brief Return the frame width.
     * @return Width in pixels, or 0 when nothing is open.
     */
    int Width() const;

    /**
     * @brief Return the frame height.
     * @return Height in pixels, or 0 when nothing is open.
     */
    int Height() const;

    /**
     * @brief Return the average frame rate ffprobe reported.
     * @return Frames per second, or 0 for raw files, still images, and when nothing is open.
     */
    double FrameRate() const;

private:
    struct StreamCloser
    {
        bool isPipe{};
        void operator()(std::FILE* file) const;
    };

    std::unique_ptr<std::FILE, StreamCloser> stream;
    int width;
    int height;
    double frameRate;
    PixelFormat format;
};

} // namespace qqr
