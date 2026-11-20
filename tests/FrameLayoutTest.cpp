#include <gtest/gtest.h>

#include "FrameLayout.hpp"
#include "Packet.hpp"

#include <cstddef>

TEST(FrameLayoutTest, FullHdSixPixelGridCarriesAboutTenKilobytes)
{
    vfdt::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(vfdt::GridCells(1920, 6), vfdt::GridCells(1080, 6), 2), vfdt::LayoutCode::Ok);
    EXPECT_EQ(layout.GridWidth(), 314);
    EXPECT_EQ(layout.GridHeight(), 174);
    EXPECT_GT(layout.PayloadBytes(), 9000U);
    EXPECT_EQ(layout.PacketBytes() * static_cast<std::size_t>(layout.PacketsPerFrame()) <= layout.PayloadBytes(), true);
    EXPECT_EQ(layout.SymbolSize() + vfdt::packet_overhead_bytes, layout.PacketBytes());
}

TEST(FrameLayoutTest, OddGridIsRejected)
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(101, 100, 2), vfdt::LayoutCode::OddGrid);
}

TEST(FrameLayoutTest, SmallGridIsRejected)
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(62, 100, 2), vfdt::LayoutCode::GridTooSmall);
}

TEST(FrameLayoutTest, ThreeBitsPerCellIsRejected)
{
    vfdt::FrameLayout layout{};
    EXPECT_EQ(layout.Configure(100, 100, 3), vfdt::LayoutCode::UnsupportedBitsPerCell);
}

TEST(FrameLayoutTest, CornersAreFindersAndCentreIsData)
{
    vfdt::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(100, 80, 2), vfdt::LayoutCode::Ok);
    EXPECT_EQ(layout.RoleAt(0, 0), vfdt::CellRole::Finder);
    EXPECT_EQ(layout.RoleAt(99, 79), vfdt::CellRole::Finder);
    EXPECT_EQ(layout.RoleAt(22, 22), vfdt::CellRole::Separator);
    EXPECT_EQ(layout.RoleAt(50, vfdt::finder_center_cell), vfdt::CellRole::Timing);
    EXPECT_EQ(layout.RoleAt(vfdt::finder_center_cell, 40), vfdt::CellRole::Timing);
    EXPECT_EQ(layout.RoleAt(50, vfdt::palette_row_offset), vfdt::CellRole::Palette);
    EXPECT_EQ(layout.RoleAt(50, vfdt::parity_row_offset), vfdt::CellRole::Parity);
    EXPECT_EQ(layout.RoleAt(50, 40), vfdt::CellRole::Data);
}

TEST(FrameLayoutTest, FinderHasLightRingAndDarkCore)
{
    vfdt::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(100, 80, 2), vfdt::LayoutCode::Ok);
    EXPECT_EQ(layout.ReservedColor(1, 10, 0), vfdt::palette_black);
    EXPECT_EQ(layout.ReservedColor(4, 10, 0), vfdt::palette_white);
    EXPECT_EQ(layout.ReservedColor(10, 10, 0), vfdt::palette_black);
}

TEST(FrameLayoutTest, ParityStripFollowsFrameNumber)
{
    vfdt::FrameLayout layout{};
    ASSERT_EQ(layout.Configure(100, 80, 2), vfdt::LayoutCode::Ok);
    EXPECT_EQ(layout.ReservedColor(50, vfdt::parity_row_offset, 0), vfdt::palette_black);
    EXPECT_EQ(layout.ReservedColor(50, vfdt::parity_row_offset, 1), vfdt::palette_white);
}
