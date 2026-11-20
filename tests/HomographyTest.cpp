#include <gtest/gtest.h>

#include "Homography.hpp"

#include <array>
#include <cstddef>

TEST(HomographyTest, FitsKnownCorners)
{
    const std::array<vfdt::Point2, 4> from{vfdt::Point2{0, 0}, vfdt::Point2{10, 0}, vfdt::Point2{0, 10}, vfdt::Point2{10, 10}};
    const std::array<vfdt::Point2, 4> to{vfdt::Point2{5, 7}, vfdt::Point2{40, 3}, vfdt::Point2{2, 30}, vfdt::Point2{45, 38}};
    vfdt::Homography mapping{};
    ASSERT_EQ(vfdt::FitHomography(from, to, mapping), vfdt::HomographyCode::Ok);
    for (std::size_t i = 0; i < 4; ++i)
    {
        const vfdt::Point2 mapped = mapping.Map(from[i]);
        EXPECT_NEAR(mapped.x, to[i].x, 1e-9);
        EXPECT_NEAR(mapped.y, to[i].y, 1e-9);
    }
}

TEST(HomographyTest, CollinearPointsAreDegenerate)
{
    const std::array<vfdt::Point2, 4> from{vfdt::Point2{0, 0}, vfdt::Point2{1, 0}, vfdt::Point2{2, 0}, vfdt::Point2{3, 0}};
    vfdt::Homography mapping{};
    EXPECT_EQ(vfdt::FitHomography(from, from, mapping), vfdt::HomographyCode::Degenerate);
}
