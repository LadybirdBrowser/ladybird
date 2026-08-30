/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>

namespace Web::Painting {

class CanvasSurfaceRegistry {
    AK_MAKE_NONCOPYABLE(CanvasSurfaceRegistry);
    AK_MAKE_DEFAULT_MOVABLE(CanvasSurfaceRegistry);

public:
    CanvasSurfaceRegistry() = default;
    ~CanvasSurfaceRegistry() = default;

    CanvasId allocate_canvas_id()
    {
        return CanvasId { m_next_canvas_id++ };
    }

    CanvasId create_canvas_surface(NonnullRefPtr<Gfx::PaintingSurface> surface)
    {
        auto id = allocate_canvas_id();
        set_canvas_surface(id, move(surface));
        return id;
    }

    void set_canvas_surface(CanvasId id, NonnullRefPtr<Gfx::PaintingSurface> surface)
    {
        m_surfaces.set(id, move(surface));
        m_content_generations.set(id, ++m_last_content_generation);
    }

    void remove_canvas_surface(CanvasId id)
    {
        m_surfaces.remove(id);
        m_content_generations.remove(id);
    }

    Gfx::PaintingSurface const* canvas_surface(CanvasId id) const
    {
        return m_surfaces.get(id).value_or(nullptr);
    }

    u64 canvas_content_generation(CanvasId id) const
    {
        return m_content_generations.get(id).value_or(0);
    }

private:
    u64 m_next_canvas_id { 1 };
    u64 m_last_content_generation { 0 };
    HashMap<CanvasId, NonnullRefPtr<Gfx::PaintingSurface>> m_surfaces;
    HashMap<CanvasId, u64> m_content_generations;
};

}
