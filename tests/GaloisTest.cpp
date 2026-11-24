#include <gtest/gtest.h>

#include "Galois.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

TEST(GaloisTest, InverseTimesValueIsOne)
{
    for (int value = 1; value < 256; ++value)
    {
        const std::uint8_t a = static_cast<std::uint8_t>(value);
        EXPECT_EQ(qqr::GfMul(a, qqr::GfInverse(a)), 1) << value;
    }
}

TEST(GaloisTest, DivisionUndoesMultiplication)
{
    EXPECT_EQ(qqr::GfDiv(qqr::GfMul(87, 200), 200), 87);
    EXPECT_EQ(qqr::GfMul(0, 200), 0);
}

TEST(GaloisTest, GeneratorWrapsAfter255)
{
    EXPECT_EQ(qqr::GfPow2(0), 1);
    EXPECT_EQ(qqr::GfPow2(8), 0x1D);
    EXPECT_EQ(qqr::GfPow2(255), 1);
    EXPECT_EQ(qqr::GfPow2(-1), qqr::GfInverse(2));
}

TEST(GaloisTest, MulAddMatchesByteWiseProduct)
{
    std::vector<std::uint8_t> target{1, 2, 3, 4};
    const std::vector<std::uint8_t> source{5, 6, 7, 8};
    std::vector<std::uint8_t> expected = target;
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        expected[i] ^= qqr::GfMul(source[i], 0x53);
    }
    qqr::GfMulAdd(target, source, 0x53);
    EXPECT_EQ(target, expected);
}

TEST(GaloisTest, ScaleByInverseRestoresRow)
{
    std::vector<std::uint8_t> row{9, 0, 255, 128};
    const std::vector<std::uint8_t> original = row;
    qqr::GfScale(row, 0x1F);
    qqr::GfScale(row, qqr::GfInverse(0x1F));
    EXPECT_EQ(row, original);
}
