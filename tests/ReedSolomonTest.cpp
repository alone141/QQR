#include <gtest/gtest.h>

#include "ReedSolomon.hpp"

#include <cstdint>
#include <random>
#include <set>
#include <span>
#include <vector>

namespace
{

constexpr int parity_bytes = 64;
constexpr int data_bytes = 191;

std::vector<std::uint8_t> MakeCodeword(const qqr::ReedSolomon& codec, int dataBytes, std::uint32_t seed)
{
    std::mt19937 random{seed};
    std::vector<std::uint8_t> codeword(static_cast<std::size_t>(dataBytes + codec.ParityBytes()));
    for (int i = 0; i < dataBytes; ++i)
    {
        codeword[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(random());
    }
    const std::span<std::uint8_t> all{codeword};
    EXPECT_EQ(codec.Encode(all.first(static_cast<std::size_t>(dataBytes)), all.subspan(static_cast<std::size_t>(dataBytes))),
              qqr::RsCode::Ok);
    return codeword;
}

void CorruptBytes(std::vector<std::uint8_t>& codeword, int count, std::uint32_t seed)
{
    std::mt19937 random{seed};
    std::set<std::size_t> positions;
    while (positions.size() < static_cast<std::size_t>(count))
    {
        positions.insert(random() % codeword.size());
    }
    for (const std::size_t position : positions)
    {
        codeword[position] ^= static_cast<std::uint8_t>(1 + random() % 255);
    }
}

} // namespace

TEST(ReedSolomonTest, CleanCodewordDecodesUnchanged)
{
    const qqr::ReedSolomon codec{parity_bytes};
    std::vector<std::uint8_t> codeword = MakeCodeword(codec, data_bytes, 1);
    const std::vector<std::uint8_t> original = codeword;
    int corrected = -1;
    EXPECT_EQ(codec.Decode(codeword, corrected), qqr::RsCode::Ok);
    EXPECT_EQ(corrected, 0);
    EXPECT_EQ(codeword, original);
}

TEST(ReedSolomonTest, CorrectsHalfTheParityInErrors)
{
    const qqr::ReedSolomon codec{parity_bytes};
    for (std::uint32_t seed = 0; seed < 20; ++seed)
    {
        std::vector<std::uint8_t> codeword = MakeCodeword(codec, data_bytes, seed);
        const std::vector<std::uint8_t> original = codeword;
        CorruptBytes(codeword, parity_bytes / 2, seed + 100);
        int corrected = 0;
        ASSERT_EQ(codec.Decode(codeword, corrected), qqr::RsCode::Ok);
        EXPECT_EQ(corrected, parity_bytes / 2);
        EXPECT_EQ(codeword, original);
    }
}

TEST(ReedSolomonTest, RejectsTooManyErrorsAndKeepsInput)
{
    const qqr::ReedSolomon codec{parity_bytes};
    std::vector<std::uint8_t> codeword = MakeCodeword(codec, data_bytes, 7);
    CorruptBytes(codeword, parity_bytes / 2 + 8, 8);
    const std::vector<std::uint8_t> corrupted = codeword;
    int corrected = 0;
    EXPECT_EQ(codec.Decode(codeword, corrected), qqr::RsCode::TooManyErrors);
    EXPECT_EQ(codeword, corrupted);
}

TEST(ReedSolomonTest, ShortenedCodewordCorrects)
{
    const qqr::ReedSolomon codec{16};
    std::vector<std::uint8_t> codeword = MakeCodeword(codec, 40, 3);
    const std::vector<std::uint8_t> original = codeword;
    CorruptBytes(codeword, 8, 4);
    int corrected = 0;
    ASSERT_EQ(codec.Decode(codeword, corrected), qqr::RsCode::Ok);
    EXPECT_EQ(codeword, original);
}

TEST(ReedSolomonTest, EncodeRejectsOversizedData)
{
    const qqr::ReedSolomon codec{parity_bytes};
    std::vector<std::uint8_t> data(200, 0);
    std::vector<std::uint8_t> parity(parity_bytes, 0);
    EXPECT_EQ(codec.Encode(data, parity), qqr::RsCode::BadLength);
}
