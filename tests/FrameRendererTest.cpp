#include <gtest/gtest.h>

#include "FrameLayout.hpp"
#include "FrameRenderer.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

constexpr int cell_pixels = 4;

vfdt::FrameLayout MakeLayout()
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(100, 80, 2), vfdt::LayoutCode::Ok);
    return layout;
}

vfdt::Rgb PixelAt(const vfdt::Image& image, int x, int y)
{
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 3;
    return vfdt::Rgb{image.pixels[index], image.pixels[index + 1], image.pixels[index + 2]};
}

} // namespace

TEST(FrameRendererTest, ImageIncludesQuietZone)
{
    const vfdt::FrameLayout layout = MakeLayout();
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const vfdt::Image image = renderer.Render(std::vector<std::uint8_t>{}, 0);
    EXPECT_EQ(image.width, (100 + 2 * vfdt::quiet_zone_cells) * cell_pixels);
    EXPECT_EQ(image.height, (80 + 2 * vfdt::quiet_zone_cells) * cell_pixels);
    EXPECT_EQ(PixelAt(image, 0, 0).r, 255);
}

TEST(FrameRendererTest, FinderCornerIsBlack)
{
    const vfdt::FrameLayout layout = MakeLayout();
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const vfdt::Image image = renderer.Render(std::vector<std::uint8_t>{}, 0);
    const int border = vfdt::quiet_zone_cells * cell_pixels;
    const vfdt::Rgb corner = PixelAt(image, border + 1, border + 1);
    EXPECT_EQ(corner.r, 0);
    EXPECT_EQ(corner.g, 0);
    EXPECT_EQ(corner.b, 0);
}

TEST(FrameRendererTest, ParityStripChangesBetweenFrames)
{
    const vfdt::FrameLayout layout = MakeLayout();
    const vfdt::FrameRenderer renderer{layout, cell_pixels};
    const int border = vfdt::quiet_zone_cells * cell_pixels;
    const int x = border + 50 * cell_pixels + 1;
    const int y = border + vfdt::parity_row_offset * cell_pixels + 1;
    EXPECT_EQ(PixelAt(renderer.Render(std::vector<std::uint8_t>{}, 0), x, y).r, 0);
    EXPECT_EQ(PixelAt(renderer.Render(std::vector<std::uint8_t>{}, 1), x, y).r, 255);
}
