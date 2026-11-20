#include <gtest/gtest.h>

#include "SymbolLayout.hpp"

#include <cstdint>
#include <vector>

TEST(SymbolLayoutTest, SplitsIntoFullBlocksAndRemainder)
{
    const std::uint32_t symbolSize = 100;
    const std::uint64_t fullBlock = static_cast<std::uint64_t>(vfdt::max_block_symbols) * symbolSize;
    const vfdt::SymbolLayout layout{2 * fullBlock + 250, symbolSize};
    ASSERT_EQ(layout.BlockCount(), 3U);
    EXPECT_EQ(layout.BlockSymbols(0), vfdt::max_block_symbols);
    EXPECT_EQ(layout.BlockOffset(2), 2 * fullBlock);
    EXPECT_EQ(layout.BlockBytes(2), 250U);
    EXPECT_EQ(layout.BlockSymbols(2), 3U);
}

TEST(SymbolLayoutTest, TinyContentIsOneBlock)
{
    const vfdt::SymbolLayout layout{5, 1000};
    EXPECT_EQ(layout.BlockCount(), 1U);
    EXPECT_EQ(layout.BlockSymbols(0), 1U);
}

TEST(SymbolLayoutTest, RepairMaskIsDeterministic)
{
    EXPECT_EQ(vfdt::RepairMask(1, 2, 3000, 1000), vfdt::RepairMask(1, 2, 3000, 1000));
    EXPECT_NE(vfdt::RepairMask(1, 2, 3000, 1000), vfdt::RepairMask(1, 2, 3001, 1000));
}

TEST(SymbolLayoutTest, RepairMaskStaysInsideBlockAndIsNeverEmpty)
{
    for (std::uint32_t esi = 1; esi < 200; ++esi)
    {
        const std::vector<std::uint64_t> mask = vfdt::RepairMask(9, 0, esi, 1);
        ASSERT_EQ(mask.size(), 1U);
        EXPECT_EQ(mask[0], 1ULL);
    }
    const std::vector<std::uint64_t> wide = vfdt::RepairMask(9, 0, 500, 70);
    ASSERT_EQ(wide.size(), 2U);
    EXPECT_EQ(wide[1] >> 6, 0ULL);
}
