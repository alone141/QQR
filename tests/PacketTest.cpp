#include <gtest/gtest.h>

#include "Packet.hpp"

#include <cstdint>
#include <vector>

namespace
{

vfdt::Packet MakePacket()
{
    vfdt::Packet packet{};
    packet.sessionId = 0xA1B2C3D4U;
    packet.contentSize = 123456789ULL;
    packet.blockIndex = 17;
    packet.esi = 1030;
    packet.symbol = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    return packet;
}

} // namespace

TEST(PacketTest, RoundTripKeepsEveryField)
{
    const vfdt::Packet packet = MakePacket();
    const std::vector<std::uint8_t> bytes = vfdt::SerializePacket(packet);
    ASSERT_EQ(bytes.size(), packet.symbol.size() + vfdt::packet_overhead_bytes);
    vfdt::Packet parsed{};
    ASSERT_EQ(vfdt::ParsePacket(bytes, parsed), vfdt::PacketCode::Ok);
    EXPECT_EQ(parsed.sessionId, packet.sessionId);
    EXPECT_EQ(parsed.contentSize, packet.contentSize);
    EXPECT_EQ(parsed.blockIndex, packet.blockIndex);
    EXPECT_EQ(parsed.esi, packet.esi);
    EXPECT_EQ(parsed.symbol, packet.symbol);
}

TEST(PacketTest, FlippedBitFailsChecksum)
{
    std::vector<std::uint8_t> bytes = vfdt::SerializePacket(MakePacket());
    bytes[30] ^= 0x10;
    vfdt::Packet parsed{};
    EXPECT_EQ(vfdt::ParsePacket(bytes, parsed), vfdt::PacketCode::BadChecksum);
}

TEST(PacketTest, TruncatedPacketFails)
{
    const std::vector<std::uint8_t> bytes = vfdt::SerializePacket(MakePacket());
    vfdt::Packet parsed{};
    EXPECT_NE(vfdt::ParsePacket(std::span<const std::uint8_t>{bytes}.first(bytes.size() - 1), parsed), vfdt::PacketCode::Ok);
}

TEST(PacketTest, TooShortIsWrongSize)
{
    const std::vector<std::uint8_t> bytes(vfdt::packet_overhead_bytes, 0);
    vfdt::Packet parsed{};
    EXPECT_EQ(vfdt::ParsePacket(bytes, parsed), vfdt::PacketCode::WrongSize);
}
