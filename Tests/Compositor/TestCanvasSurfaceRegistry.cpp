/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/Bitmap.h>
#include <LibTest/TestCase.h>

static NonnullRefPtr<Gfx::PaintingSurface> create_surface()
{
    return Gfx::PaintingSurface::create_with_size({ 4, 4 }, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
}

TEST_CASE(canvas_surfaces_resolve_only_for_their_owner)
{
    Compositor::CanvasSurfaceRegistry registry;
    auto owner = registry.allocate_owner_id();
    auto other_owner = registry.allocate_owner_id();
    auto surface = create_surface();
    auto canvas_id = registry.allocate_canvas_id();
    registry.set_canvas_surface(owner, canvas_id, surface);

    Compositor::OwnedCanvasSurfaces owned { registry, owner };
    Compositor::OwnedCanvasSurfaces other { registry, other_owner };
    EXPECT_EQ(owned.canvas_surface(canvas_id), surface.ptr());
    EXPECT_NE(owned.canvas_content_generation(canvas_id), 0u);
    EXPECT_EQ(other.canvas_surface(canvas_id), nullptr);
    EXPECT_EQ(other.canvas_content_generation(canvas_id), 0u);
}

TEST_CASE(removed_canvas_surfaces_resolve_for_nobody)
{
    Compositor::CanvasSurfaceRegistry registry;
    auto owner = registry.allocate_owner_id();
    auto canvas_id = registry.allocate_canvas_id();
    registry.set_canvas_surface(owner, canvas_id, create_surface());
    registry.remove_canvas_surface(canvas_id);

    EXPECT_EQ(registry.canvas_surface(owner, canvas_id), nullptr);
    EXPECT_EQ(registry.canvas_content_generation(owner, canvas_id), 0u);
}
