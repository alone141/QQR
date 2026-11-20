#pragma once

#include "Checksum.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vfdt
{

constexpr std::size_t max_file_name_bytes = 1024;

struct Manifest
{
    std::string fileName;
    std::uint64_t fileSize{};
    Sha256Digest fileHash{};
};

struct UnpackedContent
{
    Manifest manifest;
    std::span<const std::uint8_t> fileBytes;
};

enum class ManifestCode
{
    Ok,
    BadMagic,
    Truncated,
    SizeMismatch,
    HashMismatch,
};

/**
 * @brief Build the transferred content: a manifest (name, size, SHA-256) followed by the file bytes.
 * @param fileName Name to restore on the receiver. Longer names are cut to max_file_name_bytes.
 * @param fileBytes File contents.
 * @return The content bytes.
 */
std::vector<std::uint8_t> PackContent(std::string_view fileName, std::span<const std::uint8_t> fileBytes);

/**
 * @brief Split recovered content into manifest and file bytes, and verify the file hash.
 * @param content Content produced by PackContent.
 * @param out Receives the manifest and a view of the file bytes inside @p content.
 * @return ManifestCode::Ok when the hash matches, otherwise the failure.
 */
ManifestCode UnpackContent(std::span<const std::uint8_t> content, UnpackedContent& out);

/**
 * @brief Derive the session id from packed content.
 * @param content Content produced by PackContent.
 * @return A 32-bit id that differs between files with high probability.
 */
std::uint32_t SessionIdFor(std::span<const std::uint8_t> content);

} // namespace vfdt
