#include <gtest/gtest.h>

#include "Checksum.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

std::vector<std::uint8_t> Bytes(std::string_view text)
{
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string ToHex(const qqr::Sha256Digest& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    for (const std::uint8_t byte : digest)
    {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0F]);
    }
    return hex;
}

} // namespace

TEST(ChecksumTest, Crc32MatchesCheckValue)
{
    EXPECT_EQ(qqr::Crc32(Bytes("123456789")), 0xCBF43926U);
}

TEST(ChecksumTest, Sha256OfEmptyInput)
{
    EXPECT_EQ(ToHex(qqr::ComputeSha256({})), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(ChecksumTest, Sha256OfAbc)
{
    EXPECT_EQ(ToHex(qqr::ComputeSha256(Bytes("abc"))), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(ChecksumTest, Sha256OfTwoBlockMessage)
{
    EXPECT_EQ(ToHex(qqr::ComputeSha256(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(ChecksumTest, Sha256UpdateInPiecesMatchesOneCall)
{
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        data[i] = static_cast<std::uint8_t>(i * 7);
    }
    qqr::Sha256 hasher{};
    hasher.Update(std::span<const std::uint8_t>{data}.first(63));
    hasher.Update(std::span<const std::uint8_t>{data}.subspan(63, 1));
    hasher.Update(std::span<const std::uint8_t>{data}.subspan(64));
    EXPECT_EQ(hasher.Finish(), qqr::ComputeSha256(data));
}
