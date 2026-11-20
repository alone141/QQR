#include <gtest/gtest.h>

#include "Manifest.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

TEST(ManifestTest, RoundTripRestoresNameAndBytes)
{
    const std::vector<std::uint8_t> file{10, 20, 30, 40, 50};
    const std::vector<std::uint8_t> content = vfdt::PackContent("backup.tar.gz", file);
    vfdt::UnpackedContent unpacked{};
    ASSERT_EQ(vfdt::UnpackContent(content, unpacked), vfdt::ManifestCode::Ok);
    EXPECT_EQ(unpacked.manifest.fileName, "backup.tar.gz");
    EXPECT_EQ(unpacked.manifest.fileSize, file.size());
    EXPECT_TRUE(std::equal(unpacked.fileBytes.begin(), unpacked.fileBytes.end(), file.begin(), file.end()));
}

TEST(ManifestTest, CorruptedFileByteFailsHash)
{
    const std::vector<std::uint8_t> file{1, 2, 3, 4};
    std::vector<std::uint8_t> content = vfdt::PackContent("a.txt", file);
    content.back() ^= 0x01;
    vfdt::UnpackedContent unpacked{};
    EXPECT_EQ(vfdt::UnpackContent(content, unpacked), vfdt::ManifestCode::HashMismatch);
}

TEST(ManifestTest, WrongMagicIsRejected)
{
    std::vector<std::uint8_t> content = vfdt::PackContent("a.txt", std::vector<std::uint8_t>{1});
    content[0] = 'X';
    vfdt::UnpackedContent unpacked{};
    EXPECT_EQ(vfdt::UnpackContent(content, unpacked), vfdt::ManifestCode::BadMagic);
}

TEST(ManifestTest, EmptyFileRoundTrips)
{
    const std::vector<std::uint8_t> content = vfdt::PackContent("empty", std::vector<std::uint8_t>{});
    vfdt::UnpackedContent unpacked{};
    ASSERT_EQ(vfdt::UnpackContent(content, unpacked), vfdt::ManifestCode::Ok);
    EXPECT_TRUE(unpacked.fileBytes.empty());
}

TEST(ManifestTest, SessionIdDiffersBetweenFiles)
{
    const std::vector<std::uint8_t> first = vfdt::PackContent("a", std::vector<std::uint8_t>{1});
    const std::vector<std::uint8_t> second = vfdt::PackContent("a", std::vector<std::uint8_t>{2});
    EXPECT_NE(vfdt::SessionIdFor(first), vfdt::SessionIdFor(second));
}
