#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace qqr
{

constexpr std::uint16_t packet_magic = 0x4656;
constexpr std::uint8_t packet_version = 1;
constexpr std::size_t packet_header_bytes = 28;
constexpr std::size_t packet_overhead_bytes = 32;

struct Packet
{
    std::uint32_t sessionId{};
    std::uint64_t contentSize{};
    std::uint32_t blockIndex{};
    std::uint32_t esi{};
    std::vector<std::uint8_t> symbol;
};

enum class PacketCode
{
    Ok,
    WrongSize,
    BadChecksum,
    BadMagic,
    BadVersion,
};

/**
 * @brief Serialize @p packet into its wire form: 28-byte header, symbol, CRC-32.
 * @param packet Packet to write. Its symbol must be shorter than 65536 bytes.
 * @return packet.symbol.size() + packet_overhead_bytes bytes.
 */
std::vector<std::uint8_t> SerializePacket(const Packet& packet);

/**
 * @brief Parse and verify one packet in wire form.
 * @param bytes Exactly one serialized packet.
 * @param out Receives the packet on success.
 * @return PacketCode::Ok on success, otherwise the failure.
 */
PacketCode ParsePacket(std::span<const std::uint8_t> bytes, Packet& out);

} // namespace qqr
