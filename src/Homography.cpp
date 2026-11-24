#include "Homography.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

namespace qqr
{

namespace
{

constexpr double singular_pivot = 1e-12;

} // namespace

Homography::Homography()
    : m{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}
{
}

Homography::Homography(const std::array<double, 9>& coefficients)
    : m{coefficients}
{
}

Point2 Homography::Map(Point2 point) const
{
    const double w = m[6] * point.x + m[7] * point.y + m[8];
    return Point2{(m[0] * point.x + m[1] * point.y + m[2]) / w, (m[3] * point.x + m[4] * point.y + m[5]) / w};
}

HomographyCode FitHomography(std::span<const Point2, 4> from, std::span<const Point2, 4> to, Homography& out)
{
    std::array<std::array<double, 9>, 8> system{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        const double x = from[i].x;
        const double y = from[i].y;
        const double u = to[i].x;
        const double v = to[i].y;
        system[2 * i] = {x, y, 1.0, 0.0, 0.0, 0.0, -x * u, -y * u, u};
        system[2 * i + 1] = {0.0, 0.0, 0.0, x, y, 1.0, -x * v, -y * v, v};
    }

    for (std::size_t column = 0; column < 8; ++column)
    {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < 8; ++row)
        {
            if (std::abs(system[row][column]) > std::abs(system[pivot][column]))
            {
                pivot = row;
            }
        }
        if (std::abs(system[pivot][column]) < singular_pivot)
        {
            return HomographyCode::Degenerate;
        }
        std::swap(system[column], system[pivot]);
        for (std::size_t row = 0; row < 8; ++row)
        {
            if (row == column)
            {
                continue;
            }
            const double factor = system[row][column] / system[column][column];
            for (std::size_t k = column; k < 9; ++k)
            {
                system[row][k] -= factor * system[column][k];
            }
        }
    }

    std::array<double, 9> coefficients{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        coefficients[i] = system[i][8] / system[i][i];
    }
    coefficients[8] = 1.0;
    out = Homography{coefficients};
    return HomographyCode::Ok;
}

} // namespace qqr
