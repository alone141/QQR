#include <gtest/gtest.h>

#include "FrameLayout.hpp"
#include "Packet.hpp"

#include <cstddef>

TEST(FrameLayoutTest, FullHdSixPixelGridCarriesAboutTenKilobytes)
{
    qqr::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(qqr::GridCells(1920, 6), qqr::GridCells(1080, 6), 2), qqr::LayoutCode::Ok);
    EXPECT_EQ(layout.GridWidth(), 314);
    EXPECT_EQ(layout.GridHeight(), 174);
    EXPECT_GT(layout.PayloadBytes(), 9000U);
    EXPECT_EQ(layout.PacketBytes() * static_cast<std::size_t>(layout.PacketsPerFrame()) <= layout.PayloadBytes(), true);
    EXPECT_EQ(layout.SymbolSize() + qqr::packet_overhead_bytes, layout.PacketBytes());
}

TEST(FrameLayoutTest, OddGridIsRejected)
{
    qqr::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(101, 100, 2), qqr::LayoutCode::OddGrid);
}

TEST(FrameLayoutTest, SmallGridIsRejected)
{
    qqr::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(62, 100, 2), qqr::LayoutCode::GridTooSmall);
}

TEST(FrameLayoutTest, ThreeBitsPerCellIsRejected)
{
    qqr::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(100, 100, 3), qqr::LayoutCode::UnsupportedBitsPerCell);
}

TEST(FrameLayoutTest, CornersAreFindersAndCentreIsData)
{
    qqr::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(100, 80, 2), qqr::LayoutCode::Ok);
    EXPECT_EQ(layout.RoleAt(0, 0), qqr::CellRole::Finder);
    EXPECT_EQ(layout.RoleAt(99, 79), qqr::CellRole::Finder);
    EXPECT_EQ(layout.RoleAt(22, 22), qqr::CellRole::Separator);
    EXPECT_EQ(layout.RoleAt(50, qqr::finder_center_cell), qqr::CellRole::Timing);
    EXPECT_EQ(layout.RoleAt(qqr::finder_center_cell, 40), qqr::CellRole::Timing);
    EXPECT_EQ(layout.RoleAt(50, qqr::palette_row_offset), qqr::CellRole::Palette);
    EXPECT_EQ(layout.RoleAt(50, qqr::parity_row_offset), qqr::CellRole::Parity);
    EXPECT_EQ(layout.RoleAt(50, 40), qqr::CellRole::Data);
}

TEST(FrameLayoutTest, FinderHasLightRingAndDarkCore)
{
    qqr::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(100, 80, 2), qqr::LayoutCode::Ok);
    EXPECT_EQ(layout.ReservedColor(1, 10, 0), qqr::palette_black);
    EXPECT_EQ(layout.ReservedColor(4, 10, 0), qqr::palette_white);
    EXPECT_EQ(layout.ReservedColor(10, 10, 0), qqr::palette_black);
}

TEST(FrameLayoutTest, ParityStripFollowsFrameNumber)
{
    qqr::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(100, 80, 2), qqr::LayoutCode::Ok);
    EXPECT_EQ(layout.ReservedColor(50, qqr::parity_row_offset, 0), qqr::palette_black);
    EXPECT_EQ(layout.ReservedColor(50, qqr::parity_row_offset, 1), qqr::palette_white);
}
