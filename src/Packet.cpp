#include "Packet.hpp"
#include "Checksum.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vfdt
{

namespace
{

void PutLittleEndian(std::uint8_t* out, std::uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

std::uint64_t GetLittleEndian(const std::uint8_t* in, int bytes)
{
    std::uint64_t value = 0;
    for (int i = 0; i < bytes; ++i)
    {
        value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
    }
    return value;
}

} // namespace

std::vector<std::uint8_t> SerializePacket(const Packet& packet)
{
    const std::size_t symbolSize = packet.symbol.size();
    std::vector<std::uint8_t> bytes(symbolSize + packet_overhead_bytes, 0);
    PutLittleEndian(bytes.data(), packet_magic, 2);
    bytes[2] = packet_version;
    PutLittleEndian(bytes.data() + 4, symbolSize, 2);
    PutLittleEndian(bytes.data() + 8, packet.sessionId, 4);
    PutLittleEndian(bytes.data() + 12, packet.contentSize, 8);
    PutLittleEndian(bytes.data() + 20, packet.blockIndex, 4);
    PutLittleEndian(bytes.data() + 24, packet.esi, 4);
    std::copy(packet.symbol.begin(), packet.symbol.end(), bytes.begin() + packet_header_bytes);
    const std::uint32_t crc = Crc32(std::span<const std::uint8_t>{bytes.data(), packet_header_bytes + symbolSize});
    PutLittleEndian(bytes.data() + packet_header_bytes + symbolSize, crc, 4);
    return bytes;
}

PacketCode ParsePacket(std::span<const std::uint8_t> bytes, Packet& out)
{
    if (bytes.size() <= packet_overhead_bytes)
    {
        return PacketCode::WrongSize;
    }

    const std::size_t symbolSize = bytes.size() - packet_overhead_bytes;
    const std::uint32_t storedCrc = static_cast<std::uint32_t>(GetLittleEndian(bytes.data() + packet_header_bytes + symbolSize, 4));
    if (Crc32(bytes.first(packet_header_bytes + symbolSize)) != storedCrc)
    {
        return PacketCode::BadChecksum;
    }
    if (GetLittleEndian(bytes.data(), 2) != packet_magic)
    {
        return PacketCode::BadMagic;
    }
    if (bytes[2] != packet_version)
    {
        return PacketCode::BadVersion;
    }
    if (GetLittleEndian(bytes.data() + 4, 2) != symbolSize)
    {
        return PacketCode::WrongSize;
    }

    out.sessionId = static_cast<std::uint32_t>(GetLittleEndian(bytes.data() + 8, 4));
    out.contentSize = GetLittleEndian(bytes.data() + 12, 8);
    out.blockIndex = static_cast<std::uint32_t>(GetLittleEndian(bytes.data() + 20, 4));
    out.esi = static_cast<std::uint32_t>(GetLittleEndian(bytes.data() + 24, 4));
    out.symbol.assign(bytes.begin() + packet_header_bytes, bytes.begin() + static_cast<std::ptrdiff_t>(packet_header_bytes + symbolSize));
    return PacketCode::Ok;
}

} // namespace vfdt
