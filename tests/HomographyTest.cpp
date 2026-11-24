#include <gtest/gtest.h>

#include "Homography.hpp"

#include <array>
#include <cstddef>

TEST(HomographyTest, FitsKnownCorners)
{
    const std::array<qqr::Point2, 4> from{qqr::Point2{0, 0}, qqr::Point2{10, 0}, qqr::Point2{0, 10}, qqr::Point2{10, 10}};
    const std::array<qqr::Point2, 4> to{qqr::Point2{5, 7}, qqr::Point2{40, 3}, qqr::Point2{2, 30}, qqr::Point2{45, 38}};
    qqr::Homography mapping{};
    ASSERT_EQ(qqr::FitHomography(from, to, mapping), qqr::HomographyCode::Ok);
    for (std::size_t i = 0; i < 4; ++i)
    {
        const qqr::Point2 mapped = mapping.Map(from[i]);
        EXPECT_NEAR(mapped.x, to[i].x, 1e-9);
        EXPECT_NEAR(mapped.y, to[i].y, 1e-9);
    }
}

TEST(HomographyTest, CollinearPointsAreDegenerate)
{
    const std::array<qqr::Point2, 4> from{qqr::Point2{0, 0}, qqr::Point2{1, 0}, qqr::Point2{2, 0}, qqr::Point2{3, 0}};
    qqr::Homography mapping{};
    EXPECT_EQ(qqr::FitHomography(from, from, mapping), qqr::HomographyCode::Degenerate);
}
