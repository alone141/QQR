#include <gtest/gtest.h>

#include "FountainEncoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

std::vector<std::uint8_t> MakeContent(std::size_t size)
{
    std::vector<std::uint8_t> content(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        content[i] = static_cast<std::uint8_t>(i * 31 + 7);
    }
    return content;
}

} // namespace

TEST(FountainEncoderTest, SourceSymbolCopiesContent)
{
    const std::vector<std::uint8_t> content = MakeContent(250);
    const vfdt::FountainEncoder encoder{content, 100, 5};
    const vfdt::Packet packet = encoder.MakePacket(0, 1);
    ASSERT_EQ(packet.symbol.size(), 100U);
    EXPECT_TRUE(std::equal(packet.symbol.begin(), packet.symbol.end(), content.begin() + 100));
    EXPECT_EQ(packet.contentSize, 250U);
    EXPECT_EQ(packet.sessionId, 5U);
}

TEST(FountainEncoderTest, LastSourceSymbolIsZeroPadded)
{
    const std::vector<std::uint8_t> content = MakeContent(250);
    const vfdt::FountainEncoder encoder{content, 100, 5};
    const vfdt::Packet packet = encoder.MakePacket(0, 2);
    EXPECT_TRUE(std::equal(packet.symbol.begin(), packet.symbol.begin() + 50, content.begin() + 200));
    for (std::size_t i = 50; i < 100; ++i)
    {
        EXPECT_EQ(packet.symbol[i], 0);
    }
}

TEST(FountainEncoderTest, RepairSymbolIsXorOfMaskedSources)
{
    const std::vector<std::uint8_t> content = MakeContent(1000);
    const vfdt::FountainEncoder encoder{content, 100, 5};
    const std::uint32_t esi = 25;
    const std::vector<std::uint64_t> mask = vfdt::RepairMask(5, 0, esi, 10);
    std::vector<std::uint8_t> expected(100, 0);
    for (std::uint32_t i = 0; i < 10; ++i)
    {
        if (((mask[0] >> i) & 1ULL) != 0)
        {
            for (std::size_t j = 0; j < 100; ++j)
            {
                expected[j] ^= content[i * 100 + j];
            }
        }
    }
    EXPECT_EQ(encoder.MakePacket(0, esi).symbol, expected);
}
