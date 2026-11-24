#include <gtest/gtest.h>

#include "SymbolLayout.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

TEST(SymbolLayoutTest, SplitsIntoFullBlocksAndRemainder)
{
    const std::uint32_t symbolSize = 100;
    const std::uint64_t fullBlock = static_cast<std::uint64_t>(qqr::max_block_symbols) * symbolSize;
    const qqr::SymbolLayout layout{2 * fullBlock + 250, symbolSize};
    ASSERT_EQ(layout.BlockCount(), 3U);
    EXPECT_EQ(layout.BlockSymbols(0), qqr::max_block_symbols);
    EXPECT_EQ(layout.BlockOffset(2), 2 * fullBlock);
    EXPECT_EQ(layout.BlockBytes(2), 250U);
    EXPECT_EQ(layout.BlockSymbols(2), 3U);
}

TEST(SymbolLayoutTest, TinyContentIsOneBlock)
{
    const qqr::SymbolLayout layout{5, 1000};
    EXPECT_EQ(layout.BlockCount(), 1U);
    EXPECT_EQ(layout.BlockSymbols(0), 1U);
}

TEST(SymbolLayoutTest, RepairCoefficientsAreDeterministic)
{
    EXPECT_EQ(qqr::RepairCoefficients(1, 2, 3000, 1000), qqr::RepairCoefficients(1, 2, 3000, 1000));
    EXPECT_NE(qqr::RepairCoefficients(1, 2, 3000, 1000), qqr::RepairCoefficients(1, 2, 3001, 1000));
}

TEST(SymbolLayoutTest, LowIdsUseNonZeroCauchyCoefficients)
{
    const std::vector<std::uint8_t> coefficients = qqr::RepairCoefficients(1, 0, 20, 10);
    ASSERT_EQ(coefficients.size(), 10U);
    EXPECT_TRUE(std::none_of(coefficients.begin(), coefficients.end(), [](std::uint8_t value) { return value == 0; }));
}

TEST(SymbolLayoutTest, HighIdsUseZeroOrOneAndAreNeverEmpty)
{
    for (std::uint32_t esi = qqr::cauchy_symbol_limit; esi < qqr::cauchy_symbol_limit + 200; ++esi)
    {
        const std::vector<std::uint8_t> single = qqr::RepairCoefficients(9, 0, esi, 1);
        ASSERT_EQ(single.size(), 1U);
        EXPECT_EQ(single[0], 1);
    }
    const std::vector<std::uint8_t> wide = qqr::RepairCoefficients(9, 0, 5000, 70);
    EXPECT_TRUE(std::all_of(wide.begin(), wide.end(), [](std::uint8_t value) { return value <= 1; }));
}
