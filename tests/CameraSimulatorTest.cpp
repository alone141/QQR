#include <gtest/gtest.h>

#include "CameraSimulator.hpp"
#include "Image.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace
{

vfdt::Image MakeSolid(int width, int height, std::uint8_t value)
{
    vfdt::Image image = vfdt::MakeImage(width, height);
    std::fill(image.pixels.begin(), image.pixels.end(), value);
    return image;
}

std::uint8_t CentreRed(const vfdt::Image& image)
{
    const std::size_t index = (static_cast<std::size_t>(image.height / 2) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(image.width / 2)) * 3;
    return image.pixels[index];
}

vfdt::CameraModel MakeIdealCamera()
{
    vfdt::CameraModel model{};
    model.width = 320;
    model.height = 240;
    model.tilt = 0.0;
    model.blurSigma = 0.0;
    model.noiseSigma = 0.0;
    model.gain = {1.0, 1.0, 1.0};
    model.blackLevel = 0.0;
    model.chromaSubsampling = false;
    return model;
}

} // namespace

TEST(CameraSimulatorTest, OutputHasCameraSize)
{
    const vfdt::Image screen = MakeSolid(200, 100, 255);
    vfdt::CameraSimulator camera{MakeIdealCamera(), screen.width, screen.height};
    const vfdt::Image capture = camera.Capture(screen, screen, 1.0);
    EXPECT_EQ(capture.width, 320);
    EXPECT_EQ(capture.height, 240);
}

TEST(CameraSimulatorTest, IdealCameraSeesScreenAndBackground)
{
    const vfdt::Image screen = MakeSolid(200, 100, 200);
    vfdt::CameraSimulator camera{MakeIdealCamera(), screen.width, screen.height};
    const vfdt::Image capture = camera.Capture(screen, screen, 1.0);
    EXPECT_EQ(CentreRed(capture), 200);
    EXPECT_LT(capture.pixels[0], 60);
}

TEST(CameraSimulatorTest, SwitchBeforeCaptureShowsNextFrame)
{
    const vfdt::Image dark = MakeSolid(200, 100, 0);
    const vfdt::Image light = MakeSolid(200, 100, 255);
    vfdt::CameraModel model = MakeIdealCamera();
    model.readout = 0.0;
    vfdt::CameraSimulator camera{model, dark.width, dark.height};
    EXPECT_EQ(CentreRed(camera.Capture(dark, light, -1.0)), 255);
    EXPECT_EQ(CentreRed(camera.Capture(dark, light, 1.0)), 0);
}
