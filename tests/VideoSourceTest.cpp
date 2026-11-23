#include <gtest/gtest.h>

#include "VideoSource.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{

std::filesystem::path WriteRawFrames(int width, int height, int frames)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "vfdt_video_source_test.rgb";
    std::ofstream file{path, std::ios::binary};
    for (int f = 0; f < frames; ++f)
    {
        const std::vector<char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, static_cast<char>(f + 1));
        file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    }
    file.write("xy", 2);
    return path;
}

} // namespace

TEST(VideoSourceTest, ReadsRawFramesThenEnds)
{
    const std::filesystem::path path = WriteRawFrames(4, 3, 2);
    vfdt::VideoSource source{};
    ASSERT_EQ(source.OpenRaw(path, 4, 3), vfdt::VideoCode::Ok);
    vfdt::Image frame{};
    ASSERT_EQ(source.ReadFrame(frame), vfdt::VideoCode::Ok);
    EXPECT_EQ(frame.width, 4);
    EXPECT_EQ(frame.pixels[0], 1);
    ASSERT_EQ(source.ReadFrame(frame), vfdt::VideoCode::Ok);
    EXPECT_EQ(frame.pixels[0], 2);
    EXPECT_EQ(source.ReadFrame(frame), vfdt::VideoCode::EndOfStream);
}

TEST(VideoSourceTest, RawFramesHaveNoFrameRate)
{
    const std::filesystem::path path = WriteRawFrames(4, 3, 1);
    vfdt::VideoSource source{};
    ASSERT_EQ(source.OpenRaw(path, 4, 3), vfdt::VideoCode::Ok);
    EXPECT_EQ(source.FrameRate(), 0.0);
}

TEST(VideoSourceTest, MissingFileFailsToOpen)
{
    vfdt::VideoSource source{};
    EXPECT_EQ(source.OpenRaw(std::filesystem::temp_directory_path() / "vfdt_no_such_file.rgb", 4, 3), vfdt::VideoCode::OpenFailed);
}

TEST(VideoSourceTest, MissingToolFailsProbe)
{
    vfdt::VideoSource source{};
    EXPECT_EQ(source.OpenVideo("vfdt_no_such_ffmpeg", "vfdt_no_such_ffprobe", "video.mp4"), vfdt::VideoCode::ProbeFailed);
}
