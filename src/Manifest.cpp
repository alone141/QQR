#include "Manifest.hpp"
#include "Checksum.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qqr
{

namespace
{

constexpr std::array<std::uint8_t, 8> manifest_magic{'V', 'F', 'D', 'T', 'M', 'A', 'N', '1'};
constexpr std::size_t fixed_manifest_bytes = 8 + 2 + 8 + sha256_digest_bytes;

} // namespace

std::vector<std::uint8_t> PackContent(std::string_view fileName, std::span<const std::uint8_t> fileBytes)
{
    const std::string_view name = fileName.substr(0, std::min(fileName.size(), max_file_name_bytes));
    const Sha256Digest hash = ComputeSha256(fileBytes);

    std::vector<std::uint8_t> content;
    content.reserve(fixed_manifest_bytes + name.size() + fileBytes.size());
    content.insert(content.end(), manifest_magic.begin(), manifest_magic.end());
    content.push_back(static_cast<std::uint8_t>(name.size()));
    content.push_back(static_cast<std::uint8_t>(name.size() >> 8));
    content.insert(content.end(), name.begin(), name.end());
    for (int i = 0; i < 8; ++i)
    {
        content.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(fileBytes.size()) >> (8 * i)));
    }
    content.insert(content.end(), hash.begin(), hash.end());
    content.insert(content.end(), fileBytes.begin(), fileBytes.end());
    return content;
}

ManifestCode UnpackContent(std::span<const std::uint8_t> content, UnpackedContent& out)
{
    if (content.size() < fixed_manifest_bytes)
    {
        return ManifestCode::Truncated;
    }
    if (!std::equal(manifest_magic.begin(), manifest_magic.end(), content.begin()))
    {
        return ManifestCode::BadMagic;
    }

    const std::size_t nameLength = content[8] | (static_cast<std::size_t>(content[9]) << 8);
    if (content.size() < fixed_manifest_bytes + nameLength)
    {
        return ManifestCode::Truncated;
    }

    std::size_t offset = 10;
    Manifest manifest{};
    manifest.fileName.assign(reinterpret_cast<const char*>(content.data() + offset), nameLength);
    offset += nameLength;
    for (int i = 0; i < 8; ++i)
    {
        manifest.fileSize |= static_cast<std::uint64_t>(content[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    offset += 8;
    std::copy_n(content.begin() + static_cast<std::ptrdiff_t>(offset), sha256_digest_bytes, manifest.fileHash.begin());
    offset += sha256_digest_bytes;

    if (content.size() - offset != manifest.fileSize)
    {
        return ManifestCode::SizeMismatch;
    }
    const std::span<const std::uint8_t> fileBytes = content.subspan(offset);
    if (ComputeSha256(fileBytes) != manifest.fileHash)
    {
        return ManifestCode::HashMismatch;
    }

    out.manifest = manifest;
    out.fileBytes = fileBytes;
    return ManifestCode::Ok;
}

std::uint32_t SessionIdFor(std::span<const std::uint8_t> content)
{
    return Crc32(content.first(std::min(content.size(), fixed_manifest_bytes + max_file_name_bytes)));
}

} // namespace qqr
