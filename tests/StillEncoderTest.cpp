#include <gtest/gtest.h>

#include "CameraSimulator.hpp"
#include "FountainDecoder.hpp"
#include "FrameLayout.hpp"
#include "FrameRenderer.hpp"
#include "Manifest.hpp"
#include "Packet.hpp"
#include "Receiver.hpp"
#include "StillEncoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace
{

constexpr int cell_pixels = 6;

vfdt::FrameLayout MakeFullHdLayout()
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(vfdt::GridCells(1920, cell_pixels), vfdt::GridCells(1080, cell_pixels), 2), vfdt::LayoutCode::Ok);
    return layout;
}

std::vector<std::uint8_t> MakeFile(std::size_t size)
{
    std::mt19937 random{11};
    std::vector<std::uint8_t> file(size);
    for (std::uint8_t& byte : file)
    {
        byte = static_cast<std::uint8_t>(random());
    }
    return file;
}

int FeedPackets(const vfdt::FrameLayout& layout, const std::vector<std::uint8_t>& payload, const std::vector<int>& skipped,
                vfdt::FountainDecoder& decoder)
{
    int fed = 0;
    for (int slot = 0; slot < layout.PacketsPerFrame(); ++slot)
    {
        if (std::find(skipped.begin(), skipped.end(), slot) != skipped.end())
        {
            continue;
        }
        vfdt::Packet packet{};
        const std::span<const std::uint8_t> bytes =
            std::span<const std::uint8_t>{payload}.subspan(static_cast<std::size_t>(slot) * layout.PacketBytes(), layout.PacketBytes());
        EXPECT_EQ(vfdt::ParsePacket(bytes, packet), vfdt::PacketCode::Ok);
        decoder.AddPacket(packet);
        ++fed;
    }
    return fed;
}

} // namespace

TEST(StillEncoderTest, CapacityLeavesRoomForRepair)
{
    const vfdt::FrameLayout layout = MakeFullHdLayout();
    const std::uint64_t capacity = vfdt::StillCapacity(layout);
    EXPECT_GT(capacity, 0U);
    EXPECT_LT(capacity, static_cast<std::uint64_t>(layout.PacketsPerFrame()) * layout.SymbolSize());
}

TEST(StillEncoderTest, OneFrameRecoversContent)
{
    const vfdt::FrameLayout layout = MakeFullHdLayout();
    const std::vector<std::uint8_t> content = vfdt::PackContent("a.txt", MakeFile(3000));
    std::vector<std::uint8_t> payload;
    ASSERT_EQ(vfdt::BuildStillPayload(content, 1, layout, payload), vfdt::StillCode::Ok);
    vfdt::FountainDecoder decoder{};
    FeedPackets(layout, payload, {}, decoder);
    ASSERT_TRUE(decoder.IsComplete());
    EXPECT_TRUE(std::equal(decoder.Content().begin(), decoder.Content().end(), content.begin(), content.end()));
}

TEST(StillEncoderTest, FullStillSurvivesTwoLostPackets)
{
    const vfdt::FrameLayout layout = MakeFullHdLayout();
    const std::uint64_t capacity = vfdt::StillCapacity(layout);
    const std::vector<std::uint8_t> probe = vfdt::PackContent("b.bin", {});
    const std::vector<std::uint8_t> content = vfdt::PackContent("b.bin", MakeFile(static_cast<std::size_t>(capacity) - probe.size()));
    ASSERT_EQ(content.size(), capacity);
    std::vector<std::uint8_t> payload;
    ASSERT_EQ(vfdt::BuildStillPayload(content, 2, layout, payload), vfdt::StillCode::Ok);
    vfdt::FountainDecoder decoder{};
    FeedPackets(layout, payload, {0, 3}, decoder);
    EXPECT_TRUE(decoder.IsComplete());
}

TEST(StillEncoderTest, ContentAboveCapacityIsTooLarge)
{
    const vfdt::FrameLayout layout = MakeFullHdLayout();
    const std::vector<std::uint8_t> content(static_cast<std::size_t>(vfdt::StillCapacity(layout)) + 1, 7);
    std::vector<std::uint8_t> payload;
    EXPECT_EQ(vfdt::BuildStillPayload(content, 3, layout, payload), vfdt::StillCode::TooLarge);
}

TEST(StillEncoderTest, PhotoOfStillDecodes)
{
    const vfdt::FrameLayout layout = MakeFullHdLayout();
    const std::vector<std::uint8_t> file = MakeFile(4000);
    const std::vector<std::uint8_t> content = vfdt::PackContent("photo.bin", file);
    std::vector<std::uint8_t> payload;
    ASSERT_EQ(vfdt::BuildStillPayload(content, vfdt::SessionIdFor(content), layout, payload), vfdt::StillCode::Ok);
    const vfdt::Image screen = vfdt::FrameRenderer{layout, cell_pixels}.Render(payload, 0);

    vfdt::CameraModel model{};
    model.width = 2400;
    model.height = 1600;
    model.tilt = 0.05;
    model.blurSigma = 1.2;
    model.noiseSigma = 4.0;
    vfdt::CameraSimulator camera{model, screen.width, screen.height};
    vfdt::Receiver receiver{2, 1};
    ASSERT_EQ(receiver.AcceptImage(camera.Capture(screen, screen, 1.0)), vfdt::ReadCode::Ok);
    vfdt::UnpackedContent unpacked{};
    ASSERT_EQ(receiver.Unpack(unpacked), vfdt::ReceiveCode::Ok);
    EXPECT_EQ(unpacked.manifest.fileName, "photo.bin");
    EXPECT_TRUE(std::equal(unpacked.fileBytes.begin(), unpacked.fileBytes.end(), file.begin(), file.end()));
}
