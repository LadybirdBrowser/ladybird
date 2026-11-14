/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/MachPort.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibTest/TestCase.h>
#include <LibWebView/PageBackingStores.h>

static Vector<Gfx::SharedImageBuffer> s_buffers;

static Gfx::SharedImage importable_image()
{
    s_buffers.append(Gfx::SharedImageBuffer::create({ 4, 4 }));
    return s_buffers.last().export_shared_image();
}

// A send right to a port that isn't an IOSurface's.
static Gfx::SharedImage unimportable_image()
{
    auto receive_right = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    return Gfx::SharedImage { MUST(receive_right.insert_right(Core::MachPort::MessageRight::MakeSend)) };
}

static Vector<Gfx::SharedImage> images(Vector<bool> importable)
{
    Vector<Gfx::SharedImage> images;
    for (auto is_importable : importable)
        images.append(is_importable ? importable_image() : unimportable_image());
    return images;
}

TEST_CASE(a_refused_installation_keeps_the_backing_stores_on_display)
{
    WebView::PageBackingStores backing_stores;
    TRY_OR_FAIL(backing_stores.install({ 1, 2 }, images({ true, true })));
    EXPECT_EQ(backing_stores.present(2, { 4, 4 }, 1).bitmap_id_to_release, 1);

    EXPECT(backing_stores.install({ 10, 11 }, images({ true, false })).is_error());
    EXPECT_EQ(backing_stores.front_bitmap_id(), 2);
    EXPECT(backing_stores.has_usable_front_bitmap());
    EXPECT_EQ(backing_stores.displayed_frame()->bitmap_id, 2);
}

TEST_CASE(the_backing_stores_of_a_refused_installation_are_handed_back)
{
    WebView::PageBackingStores backing_stores;
    TRY_OR_FAIL(backing_stores.install({ 1, 2 }, images({ true, true })));
    EXPECT(backing_stores.install({ 1, 2 }, images({ false, true })).is_error());

    // Each of these is for the refused installation, even where its ID is one of the installed backing stores'.
    EXPECT(backing_stores.add({ 3 }, images({ true })).is_error());
    auto result = backing_stores.present(2, { 4, 4 }, 1);
    EXPECT(!result.did_swap);
    EXPECT_EQ(result.bitmap_id_to_release, 2);
    backing_stores.retire(Vector<i32> { 2 });

    // An installation that succeeds ends the refusal.
    TRY_OR_FAIL(backing_stores.install({ 20, 21 }, images({ true, true })));
    TRY_OR_FAIL(backing_stores.add({ 22 }, images({ true })));
    result = backing_stores.present(22, { 4, 4 }, 1);
    EXPECT(result.did_swap);
    EXPECT_EQ(result.bitmap_id_to_release, 20);
}

TEST_CASE(a_refused_addition_changes_nothing)
{
    WebView::PageBackingStores backing_stores;
    TRY_OR_FAIL(backing_stores.install({ 1, 2 }, images({ true, true })));
    EXPECT(backing_stores.add({ 3, 4 }, images({ true, false })).is_error());
    EXPECT_EQ(backing_stores.present(3, { 4, 4 }, 1).bitmap_id_to_release, 3);

    // A refused addition doesn't refuse the installation it adds to.
    EXPECT(backing_stores.present(2, { 4, 4 }, 1).did_swap);
}
