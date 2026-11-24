#pragma once

#include <array>
#include <span>

namespace qqr
{

struct Point2
{
    double x{};
    double y{};
};

enum class HomographyCode
{
    Ok,
    Degenerate,
};

class Homography
{
public:
    /**
     * @brief Build the identity mapping.
     */
    Homography();

    /**
     * @brief Build a mapping from its nine row-major coefficients.
     * @param coefficients 3x3 matrix, row-major.
     */
    explicit Homography(const std::array<double, 9>& coefficients);

    /**
     * @brief Map one point.
     * @param point Point in the source plane.
     * @return The point in the target plane.
     */
    Point2 Map(Point2 point) const;

private:
    std::array<double, 9> m;
};

/**
 * @brief Fit the perspective mapping that sends each @p from point to the matching @p to point.
 * @param from Four source points, no three collinear.
 * @param to Four target points.
 * @param out Receives the mapping on success.
 * @return HomographyCode::Ok on success, HomographyCode::Degenerate when the points do not define one.
 */
HomographyCode FitHomography(std::span<const Point2, 4> from, std::span<const Point2, 4> to, Homography& out);

} // namespace qqr
