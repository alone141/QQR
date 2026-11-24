#include <gtest/gtest.h>

#include "Manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

TEST(ManifestTest, RoundTripRestoresNameAndBytes)
{
    const std::vector<std::uint8_t> file{10, 20, 30, 40, 50};
    const std::vector<std::uint8_t> content = qqr::PackContent("backup.tar.gz", file);
    qqr::UnpackedContent unpacked{};
    ASSERT_EQ(qqr::UnpackContent(content, unpacked), qqr::ManifestCode::Ok);
    EXPECT_EQ(unpacked.manifest.fileName, "backup.tar.gz");
    EXPECT_EQ(unpacked.manifest.fileSize, file.size());
    EXPECT_TRUE(std::equal(unpacked.fileBytes.begin(), unpacked.fileBytes.end(), file.begin(), file.end()));
}

TEST(ManifestTest, CorruptedFileByteFailsHash)
{
    const std::vector<std::uint8_t> file{1, 2, 3, 4};
    std::vector<std::uint8_t> content = qqr::PackContent("a.txt", file);
    content.back() ^= 0x01;
    qqr::UnpackedContent unpacked{};
    EXPECT_EQ(qqr::UnpackContent(content, unpacked), qqr::ManifestCode::HashMismatch);
}

TEST(ManifestTest, WrongMagicIsRejected)
{
    std::vector<std::uint8_t> content = qqr::PackContent("a.txt", std::vector<std::uint8_t>{1});
    content[0] = 'X';
    qqr::UnpackedContent unpacked{};
    EXPECT_EQ(qqr::UnpackContent(content, unpacked), qqr::ManifestCode::BadMagic);
}

TEST(ManifestTest, EmptyFileRoundTrips)
{
    const std::vector<std::uint8_t> content = qqr::PackContent("empty", std::vector<std::uint8_t>{});
    qqr::UnpackedContent unpacked{};
    ASSERT_EQ(qqr::UnpackContent(content, unpacked), qqr::ManifestCode::Ok);
    EXPECT_TRUE(unpacked.fileBytes.empty());
}

TEST(ManifestTest, SessionIdDiffersBetweenFiles)
{
    const std::vector<std::uint8_t> first = qqr::PackContent("a", std::vector<std::uint8_t>{1});
    const std::vector<std::uint8_t> second = qqr::PackContent("a", std::vector<std::uint8_t>{2});
    EXPECT_NE(qqr::SessionIdFor(first), qqr::SessionIdFor(second));
}
