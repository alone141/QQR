#include "CameraSimulator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace qqr
{

namespace
{

constexpr std::array<float, 3> background_color{40.0F, 40.0F, 45.0F};
constexpr double min_blur_sigma = 0.05;

// Approximate standard normal noise: the sum of four uniforms is close enough and far cheaper than std::normal_distribution.
class FastNoise
{
public:
    explicit FastNoise(std::uint32_t seed)
        : state{seed == 0 ? 0x9E3779B9U : seed}
    {
    }

    float Next()
    {
        float sum = 0.0F;
        for (int i = 0; i < 4; ++i)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            sum += static_cast<float>(state) * (1.0F / 4294967296.0F);
        }
        return (sum - 2.0F) * 1.7320508F;
    }

private:
    std::uint32_t state;
};

void GaussianBlur(std::vector<float>& buffer, int width, int height, double sigma)
{
    const int radius = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<float> kernel(static_cast<std::size_t>(2 * radius + 1));
    float total = 0.0F;
    for (int i = -radius; i <= radius; ++i)
    {
        const float weight = static_cast<float>(std::exp(-(i * i) / (2.0 * sigma * sigma)));
        kernel[static_cast<std::size_t>(i + radius)] = weight;
        total += weight;
    }
    for (float& weight : kernel)
    {
        weight /= total;
    }

    std::vector<float> temp(buffer.size(), 0.0F);
    const std::size_t stride = static_cast<std::size_t>(width) * 3;
    for (int y = 0; y < height; ++y)
    {
        const float* source = buffer.data() + static_cast<std::size_t>(y) * stride;
        float* target = temp.data() + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < width; ++x)
        {
            std::array<float, 3> sum{};
            for (int k = -radius; k <= radius; ++k)
            {
                const std::size_t sx = static_cast<std::size_t>(std::clamp(x + k, 0, width - 1));
                const float weight = kernel[static_cast<std::size_t>(k + radius)];
                for (std::size_t c = 0; c < 3; ++c)
                {
                    sum[c] += weight * source[sx * 3 + c];
                }
            }
            for (std::size_t c = 0; c < 3; ++c)
            {
                target[static_cast<std::size_t>(x) * 3 + c] = sum[c];
            }
        }
    }
    for (int y = 0; y < height; ++y)
    {
        float* target = buffer.data() + static_cast<std::size_t>(y) * stride;
        std::fill(target, target + stride, 0.0F);
        for (int k = -radius; k <= radius; ++k)
        {
            const std::size_t sy = static_cast<std::size_t>(std::clamp(y + k, 0, height - 1));
            const float weight = kernel[static_cast<std::size_t>(k + radius)];
            const float* source = temp.data() + sy * stride;
            for (std::size_t i = 0; i < stride; ++i)
            {
                target[i] += weight * source[i];
            }
        }
    }
}

void SubsampleChroma(std::vector<float>& buffer, int width, int height)
{
    for (int by = 0; by < height; by += 2)
    {
        for (int bx = 0; bx < width; bx += 2)
        {
            std::array<std::size_t, 4> indexes{};
            std::array<float, 4> lumas{};
            int count = 0;
            float cb = 0.0F;
            float cr = 0.0F;
            for (int dy = 0; dy < 2 && by + dy < height; ++dy)
            {
                for (int dx = 0; dx < 2 && bx + dx < width; ++dx)
                {
                    const std::size_t index = (static_cast<std::size_t>(by + dy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(bx + dx)) * 3;
                    const float r = buffer[index];
                    const float g = buffer[index + 1];
                    const float b = buffer[index + 2];
                    indexes[static_cast<std::size_t>(count)] = index;
                    lumas[static_cast<std::size_t>(count)] = 0.299F * r + 0.587F * g + 0.114F * b;
                    cb += -0.168736F * r - 0.331264F * g + 0.5F * b;
                    cr += 0.5F * r - 0.418688F * g - 0.081312F * b;
                    ++count;
                }
            }
            cb /= static_cast<float>(count);
            cr /= static_cast<float>(count);
            for (int i = 0; i < count; ++i)
            {
                const std::size_t index = indexes[static_cast<std::size_t>(i)];
                const float luma = lumas[static_cast<std::size_t>(i)];
                buffer[index] = luma + 1.402F * cr;
                buffer[index + 1] = luma - 0.344136F * cb - 0.714136F * cr;
                buffer[index + 2] = luma + 1.772F * cb;
            }
        }
    }
}

} // namespace

CameraSimulator::CameraSimulator(const CameraModel& model, int screenWidth, int screenHeight)
    : model{model}
    , screenWidth{screenWidth}
    , screenHeight{screenHeight}
    , cameraToScreen{}
    , random{model.seed}
{
    const double aspect = static_cast<double>(screenHeight) / screenWidth;
    double placedWidth = model.coverage * model.width;
    double placedHeight = placedWidth * aspect;
    if (placedHeight > model.coverage * model.height)
    {
        placedHeight = model.coverage * model.height;
        placedWidth = placedHeight / aspect;
    }
    const double left = (model.width - placedWidth) / 2.0;
    const double top = (model.height - placedHeight) / 2.0;
    std::uniform_real_distribution<double> jitter{-model.tilt * placedWidth, model.tilt * placedWidth};

    std::array<Point2, 4> cameraCorners{
        Point2{left, top},
        Point2{left + placedWidth, top},
        Point2{left, top + placedHeight},
        Point2{left + placedWidth, top + placedHeight},
    };
    for (Point2& corner : cameraCorners)
    {
        corner.x += jitter(random);
        corner.y += jitter(random);
    }
    const std::array<Point2, 4> screenCorners{
        Point2{0.0, 0.0},
        Point2{static_cast<double>(screenWidth), 0.0},
        Point2{0.0, static_cast<double>(screenHeight)},
        Point2{static_cast<double>(screenWidth), static_cast<double>(screenHeight)},
    };
    FitHomography(cameraCorners, screenCorners, cameraToScreen);
}

Image CameraSimulator::Capture(const Image& current, const Image& next, double switchTime)
{
    const int width = model.width;
    const int height = model.height;
    std::vector<float> buffer(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0.0F);

    // Optics: each row integrates the screen over the exposure, starting later further down (rolling shutter).
    for (int y = 0; y < height; ++y)
    {
        const double rowStart = model.readout * y / std::max(1, height - 1);
        const double nextWeight = std::clamp((rowStart + model.exposure - switchTime) / model.exposure, 0.0, 1.0);
        float* row = buffer.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3;
        for (int x = 0; x < width; ++x)
        {
            const Point2 point = cameraToScreen.Map(Point2{x + 0.5, y + 0.5});
            float* pixel = row + static_cast<std::size_t>(x) * 3;
            if (point.x < 0.0 || point.y < 0.0 || point.x >= screenWidth || point.y >= screenHeight)
            {
                std::copy(background_color.begin(), background_color.end(), pixel);
                continue;
            }
            const std::array<double, 3> first = nextWeight < 1.0 ? SampleBilinear(current, point.x, point.y) : std::array<double, 3>{};
            const std::array<double, 3> second = nextWeight > 0.0 ? SampleBilinear(next, point.x, point.y) : std::array<double, 3>{};
            for (std::size_t c = 0; c < 3; ++c)
            {
                pixel[c] = static_cast<float>(first[c] * (1.0 - nextWeight) + second[c] * nextWeight);
            }
        }
    }
    if (model.blurSigma > min_blur_sigma)
    {
        GaussianBlur(buffer, width, height, model.blurSigma);
    }

    // Sensor: colour gain, raised black level, noise.
    FastNoise noise{static_cast<std::uint32_t>(random())};
    const float span = static_cast<float>(255.0 - model.blackLevel) / 255.0F;
    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
        const float gain = static_cast<float>(model.gain[i % 3]);
        buffer[i] = static_cast<float>(model.blackLevel) + buffer[i] * gain * span + noise.Next() * static_cast<float>(model.noiseSigma);
    }

    // Encoder: 4:2:0 chroma, then 8-bit output.
    if (model.chromaSubsampling)
    {
        SubsampleChroma(buffer, width, height);
    }
    Image image = MakeImage(width, height);
    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
        image.pixels[i] = static_cast<std::uint8_t>(std::clamp(std::lround(buffer[i]), 0L, 255L));
    }
    return image;
}

} // namespace qqr
