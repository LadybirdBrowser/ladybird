/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>

namespace Compositor {

// NB: One registry holds the canvases of every WebContent that shares a compositor state. A canvas ID that a display
//     list or command stream names only resolves to a surface of the owner it is resolved for, so a WebContent cannot
//     draw (and then read back) the canvases of another.
class CanvasSurfaceRegistry {
    AK_MAKE_NONCOPYABLE(CanvasSurfaceRegistry);
    AK_MAKE_DEFAULT_MOVABLE(CanvasSurfaceRegistry);

public:
    CanvasSurfaceRegistry() = default;
    ~CanvasSurfaceRegistry() = default;

    Compositing::CanvasSurfaceOwnerId allocate_owner_id()
    {
        return Compositing::CanvasSurfaceOwnerId { m_next_owner_id++ };
    }

    Compositing::CanvasId allocate_canvas_id()
    {
        return reserve_canvas_ids(1);
    }

    Compositing::CanvasId reserve_canvas_ids(u64 count)
    {
        auto first = Compositing::CanvasId { m_next_canvas_id };
        m_next_canvas_id += count;
        return first;
    }

    void set_canvas_surface(Compositing::CanvasSurfaceOwnerId owner_id, Compositing::CanvasId id, NonnullRefPtr<Gfx::PaintingSurface> surface)
    {
        if (auto existing = m_entries.get(id); existing.has_value())
            VERIFY(existing->owner_id == owner_id);
        m_entries.set(id, { .owner_id = owner_id, .surface = move(surface), .content_generation = ++m_last_content_generation });
    }

    void remove_canvas_surface(Compositing::CanvasId id)
    {
        m_entries.remove(id);
    }

    Gfx::PaintingSurface const* canvas_surface(Compositing::CanvasSurfaceOwnerId owner_id, Compositing::CanvasId id) const
    {
        auto const* entry = entry_of(owner_id, id);
        return entry ? entry->surface.ptr() : nullptr;
    }

    u64 canvas_content_generation(Compositing::CanvasSurfaceOwnerId owner_id, Compositing::CanvasId id) const
    {
        auto const* entry = entry_of(owner_id, id);
        return entry ? entry->content_generation : 0;
    }

private:
    struct Entry {
        Compositing::CanvasSurfaceOwnerId owner_id;
        NonnullRefPtr<Gfx::PaintingSurface> surface;
        u64 content_generation { 0 };
    };

    Entry const* entry_of(Compositing::CanvasSurfaceOwnerId owner_id, Compositing::CanvasId id) const
    {
        auto it = m_entries.find(id);
        if (it == m_entries.end() || it->value.owner_id != owner_id)
            return nullptr;
        return &it->value;
    }

    u64 m_next_owner_id { 1 };
    u64 m_next_canvas_id { 1 };
    u64 m_last_content_generation { 0 };
    HashMap<Compositing::CanvasId, Entry> m_entries;
};

// The canvases of one owner, which is what a display list of that owner's contexts may draw.
class OwnedCanvasSurfaces {
public:
    OwnedCanvasSurfaces(CanvasSurfaceRegistry const& registry, Compositing::CanvasSurfaceOwnerId owner_id)
        : m_registry(&registry)
        , m_owner_id(owner_id)
    {
    }

    Gfx::PaintingSurface const* canvas_surface(Compositing::CanvasId id) const { return m_registry->canvas_surface(m_owner_id, id); }
    u64 canvas_content_generation(Compositing::CanvasId id) const { return m_registry->canvas_content_generation(m_owner_id, id); }

private:
    CanvasSurfaceRegistry const* m_registry { nullptr };
    Compositing::CanvasSurfaceOwnerId m_owner_id;
};

}
