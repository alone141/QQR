#include <gtest/gtest.h>

#include "FountainEncoder.hpp"
#include "Galois.hpp"

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
    const qqr::FountainEncoder encoder{content, 100, 5};
    const qqr::Packet packet = encoder.MakePacket(0, 1);
    ASSERT_EQ(packet.symbol.size(), 100U);
    EXPECT_TRUE(std::equal(packet.symbol.begin(), packet.symbol.end(), content.begin() + 100));
    EXPECT_EQ(packet.contentSize, 250U);
    EXPECT_EQ(packet.sessionId, 5U);
}

TEST(FountainEncoderTest, LastSourceSymbolIsZeroPadded)
{
    const std::vector<std::uint8_t> content = MakeContent(250);
    const qqr::FountainEncoder encoder{content, 100, 5};
    const qqr::Packet packet = encoder.MakePacket(0, 2);
    EXPECT_TRUE(std::equal(packet.symbol.begin(), packet.symbol.begin() + 50, content.begin() + 200));
    for (std::size_t i = 50; i < 100; ++i)
    {
        EXPECT_EQ(packet.symbol[i], 0);
    }
}

TEST(FountainEncoderTest, RepairSymbolIsWeightedSumOfSources)
{
    const std::vector<std::uint8_t> content = MakeContent(1000);
    const qqr::FountainEncoder encoder{content, 100, 5};
    for (const std::uint32_t esi : {25U, 3000U})
    {
        const std::vector<std::uint8_t> coefficients = qqr::RepairCoefficients(5, 0, esi, 10);
        std::vector<std::uint8_t> expected(100, 0);
        for (std::uint32_t i = 0; i < 10; ++i)
        {
            for (std::size_t j = 0; j < 100; ++j)
            {
                expected[j] ^= qqr::GfMul(coefficients[i], content[i * 100 + j]);
            }
        }
        EXPECT_EQ(encoder.MakePacket(0, esi).symbol, expected) << "esi " << esi;
    }
}
