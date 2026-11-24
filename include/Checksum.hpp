#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qqr
{

constexpr std::size_t sha256_digest_bytes = 32;

using Sha256Digest = std::array<std::uint8_t, sha256_digest_bytes>;

/**
 * @brief Compute the CRC-32 (IEEE 802.3, reflected) of @p data.
 * @param data Bytes to checksum.
 * @return The CRC-32 value.
 */
std::uint32_t Crc32(std::span<const std::uint8_t> data);

class Sha256
{
public:
    /**
     * @brief Start a new SHA-256 computation.
     */
    Sha256();

    /**
     * @brief Add more bytes to the hash.
     * @param data Bytes to add.
     */
    void Update(std::span<const std::uint8_t> data);

    /**
     * @brief Finish the hash. Do not call Update afterwards.
     * @return The digest of every byte passed to Update.
     */
    Sha256Digest Finish();

private:
    void ProcessChunk(const std::uint8_t* chunk);

    std::array<std::uint32_t, 8> state;
    std::array<std::uint8_t, 64> buffer;
    std::size_t bufferLength;
    std::uint64_t totalBytes;
};

/**
 * @brief Hash @p data in one call.
 * @param data Bytes to hash.
 * @return The SHA-256 digest.
 */
Sha256Digest ComputeSha256(std::span<const std::uint8_t> data);

} // namespace qqr
