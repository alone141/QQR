#pragma once

#include "Homography.hpp"
#include "Image.hpp"

#include <array>
#include <cstdint>
#include <random>

namespace vfdt
{

struct CameraModel
{
    int width{3840};
    int height{2160};
    double coverage{0.85};
    double tilt{0.03};
    double blurSigma{1.0};
    double noiseSigma{3.0};
    std::array<double, 3> gain{0.92, 0.95, 0.88};
    double blackLevel{14.0};
    double exposure{1.0 / 240.0};
    double readout{1.0 / 90.0};
    bool chromaSubsampling{true};
    std::uint32_t seed{1};
};

class CameraSimulator
{
public:
    /**
     * @brief Place a screen of the given size in the camera view, slightly tilted.
     * @param model Camera properties.
     * @param screenWidth Screen image width in pixels.
     * @param screenHeight Screen image height in pixels.
     */
    CameraSimulator(const CameraModel& model, int screenWidth, int screenHeight);

    /**
     * @brief Capture one camera frame while the screen switches from @p current to @p next.
     * @param current Screen image shown when the capture starts.
     * @param next Screen image shown after @p switchTime.
     * @param switchTime Seconds from the start of the capture until the screen switches. Rolling shutter reads rows later further down.
     * @return The captured image: perspective, blur, colour gain, noise and 4:2:0 chroma applied.
     */
    Image Capture(const Image& current, const Image& next, double switchTime);

private:
    CameraModel model;
    int screenWidth;
    int screenHeight;
    Homography cameraToScreen;
    std::mt19937 random;
};

} // namespace vfdt
