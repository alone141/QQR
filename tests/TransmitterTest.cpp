#include <gtest/gtest.h>

#include "FountainDecoder.hpp"
#include "FrameLayout.hpp"
#include "Packet.hpp"
#include "Transmitter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

vfdt::FrameLayout MakeLayout()
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(120, 80, 2), vfdt::LayoutCode::Ok);
    return layout;
}

std::vector<std::uint8_t> MakeContent(std::size_t size)
{
    std::vector<std::uint8_t> content(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        content[i] = static_cast<std::uint8_t>(i * 13 + 1);
    }
    return content;
}

void FeedFrame(const vfdt::FrameLayout& layout, const std::vector<std::uint8_t>& payload, vfdt::FountainDecoder& decoder)
{
    for (int slot = 0; slot < layout.PacketsPerFrame(); ++slot)
    {
        vfdt::Packet packet{};
        const std::span<const std::uint8_t> bytes = std::span<const std::uint8_t>{payload}.subspan(
            static_cast<std::size_t>(slot) * layout.PacketBytes(), layout.PacketBytes());
        ASSERT_EQ(vfdt::ParsePacket(bytes, packet), vfdt::PacketCode::Ok);
        decoder.AddPacket(packet);
    }
}

} // namespace

TEST(TransmitterTest, OnePassDeliversEverything)
{
    const vfdt::FrameLayout layout = MakeLayout();
    const std::vector<std::uint8_t> content = MakeContent(300000);
    const vfdt::Transmitter transmitter{content, 42, layout, 0.25};
    vfdt::FountainDecoder decoder{};
    for (std::uint64_t frame = 0; frame < transmitter.FramesPerPass(); ++frame)
    {
        FeedFrame(layout, transmitter.FramePayload(frame), decoder);
    }
    ASSERT_TRUE(decoder.IsComplete());
    EXPECT_TRUE(std::equal(decoder.Content().begin(), decoder.Content().end(), content.begin(), content.end()));
}

TEST(TransmitterTest, SecondPassAloneDeliversEverything)
{
    const vfdt::FrameLayout layout = MakeLayout();
    const std::vector<std::uint8_t> content = MakeContent(50000);
    const vfdt::Transmitter transmitter{content, 42, layout, 0.25};
    vfdt::FountainDecoder decoder{};
    const std::uint64_t pass = transmitter.FramesPerPass();
    for (std::uint64_t frame = pass; frame < 3 * pass && !decoder.IsComplete(); ++frame)
    {
        FeedFrame(layout, transmitter.FramePayload(frame), decoder);
    }
    EXPECT_TRUE(decoder.IsComplete());
}

TEST(TransmitterTest, PayloadHasLayoutSize)
{
    const vfdt::FrameLayout layout = MakeLayout();
    const std::vector<std::uint8_t> content = MakeContent(1000);
    const vfdt::Transmitter transmitter{content, 42, layout, 0.25};
    EXPECT_EQ(transmitter.FramePayload(0).size(), layout.PayloadBytes());
}
