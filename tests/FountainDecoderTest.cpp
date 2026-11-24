#include <gtest/gtest.h>

#include "FountainDecoder.hpp"
#include "FountainEncoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace
{

constexpr std::uint32_t symbol_size = 64;
constexpr std::uint32_t session_id = 77;

std::vector<std::uint8_t> MakeContent(std::size_t size, std::uint32_t seed)
{
    std::mt19937 random{seed};
    std::vector<std::uint8_t> content(size);
    for (std::uint8_t& byte : content)
    {
        byte = static_cast<std::uint8_t>(random());
    }
    return content;
}

bool ContentMatches(const qqr::FountainDecoder& decoder, const std::vector<std::uint8_t>& content)
{
    const std::span<const std::uint8_t> recovered = decoder.Content();
    return std::equal(recovered.begin(), recovered.end(), content.begin(), content.end());
}

} // namespace

TEST(FountainDecoderTest, SourceSymbolsAloneRecoverContent)
{
    const std::vector<std::uint8_t> content = MakeContent(5000, 1);
    const qqr::FountainEncoder encoder{content, symbol_size, session_id};
    qqr::FountainDecoder decoder{};
    for (std::uint32_t esi = 0; esi < encoder.Layout().BlockSymbols(0); ++esi)
    {
        decoder.AddPacket(encoder.MakePacket(0, esi));
    }
    ASSERT_TRUE(decoder.IsComplete());
    EXPECT_TRUE(ContentMatches(decoder, content));
}

TEST(FountainDecoderTest, RepairSymbolsFillLostSources)
{
    const std::vector<std::uint8_t> content = MakeContent(20000, 2);
    const qqr::FountainEncoder encoder{content, symbol_size, session_id};
    const std::uint32_t blockSymbols = encoder.Layout().BlockSymbols(0);
    qqr::FountainDecoder decoder{};
    std::mt19937 random{3};
    for (std::uint32_t esi = 0; esi < 2 * blockSymbols && !decoder.IsComplete(); ++esi)
    {
        if (random() % 100 < 30)
        {
            continue;
        }
        decoder.AddPacket(encoder.MakePacket(0, esi));
    }
    ASSERT_TRUE(decoder.IsComplete());
    EXPECT_TRUE(ContentMatches(decoder, content));
}

TEST(FountainDecoderTest, RepairSymbolsAloneRecoverContent)
{
    const std::vector<std::uint8_t> content = MakeContent(3000, 4);
    const qqr::FountainEncoder encoder{content, symbol_size, session_id};
    const std::uint32_t blockSymbols = encoder.Layout().BlockSymbols(0);
    qqr::FountainDecoder decoder{};
    std::uint32_t used = 0;
    for (std::uint32_t esi = blockSymbols; esi < 3 * blockSymbols && !decoder.IsComplete(); ++esi)
    {
        decoder.AddPacket(encoder.MakePacket(0, esi));
        ++used;
    }
    ASSERT_TRUE(decoder.IsComplete());
    EXPECT_LE(used, blockSymbols + 20);
    EXPECT_TRUE(ContentMatches(decoder, content));
}

TEST(FountainDecoderTest, SmallBlockRecoversFromAnyKSymbols)
{
    const std::vector<std::uint8_t> content = MakeContent(10 * symbol_size, 8);
    const qqr::FountainEncoder encoder{content, symbol_size, session_id};
    const std::uint32_t blockSymbols = encoder.Layout().BlockSymbols(0);
    ASSERT_EQ(blockSymbols, 10U);
    std::mt19937 random{9};
    for (int trial = 0; trial < 50; ++trial)
    {
        // Lose a random number of sources and use exactly that many repair symbols: no spare.
        std::vector<std::uint32_t> ids(blockSymbols + 6);
        for (std::uint32_t i = 0; i < ids.size(); ++i)
        {
            ids[i] = i;
        }
        std::shuffle(ids.begin(), ids.end(), random);
        qqr::FountainDecoder decoder{};
        for (std::uint32_t i = 0; i < blockSymbols; ++i)
        {
            decoder.AddPacket(encoder.MakePacket(0, ids[i]));
        }
        ASSERT_TRUE(decoder.IsComplete()) << "trial " << trial;
        EXPECT_TRUE(ContentMatches(decoder, content));
    }
}

TEST(FountainDecoderTest, MultipleBlocksCompleteIndependently)
{
    const std::size_t size = static_cast<std::size_t>(qqr::max_block_symbols) * symbol_size * 2 + 100;
    const std::vector<std::uint8_t> content = MakeContent(size, 5);
    const qqr::FountainEncoder encoder{content, symbol_size, session_id};
    ASSERT_EQ(encoder.Layout().BlockCount(), 3U);
    qqr::FountainDecoder decoder{};
    for (const std::uint32_t block : {2U, 0U})
    {
        for (std::uint32_t esi = 0; esi < encoder.Layout().BlockSymbols(block); ++esi)
        {
            decoder.AddPacket(encoder.MakePacket(block, esi));
        }
    }
    EXPECT_EQ(decoder.CompletedBlocks(), 2U);
    EXPECT_EQ(decoder.MissingBlocks(), std::vector<std::uint32_t>{1U});
    EXPECT_FALSE(decoder.IsComplete());
}

TEST(FountainDecoderTest, DuplicatePacketIsReported)
{
    const std::vector<std::uint8_t> content = MakeContent(1000, 6);
    const qqr::FountainEncoder encoder{content, symbol_size, session_id};
    qqr::FountainDecoder decoder{};
    EXPECT_EQ(decoder.AddPacket(encoder.MakePacket(0, 0)), qqr::AddCode::Ok);
    EXPECT_EQ(decoder.AddPacket(encoder.MakePacket(0, 0)), qqr::AddCode::Duplicate);
}

TEST(FountainDecoderTest, OtherSessionIsRejected)
{
    const std::vector<std::uint8_t> content = MakeContent(1000, 7);
    const qqr::FountainEncoder first{content, symbol_size, session_id};
    const qqr::FountainEncoder second{content, symbol_size, session_id + 1};
    qqr::FountainDecoder decoder{};
    EXPECT_EQ(decoder.AddPacket(first.MakePacket(0, 0)), qqr::AddCode::Ok);
    EXPECT_EQ(decoder.AddPacket(second.MakePacket(0, 1)), qqr::AddCode::SessionMismatch);
}
