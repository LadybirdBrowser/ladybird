/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibPaintServer/Types.h>

class SkTypeface;
class SkFont;

namespace PaintServer {

void set_force_fontconfig(bool);

// Cache mapping per-surface font_resource_id to SkTypeface for one render client.
class FontResourceCache {
    AK_MAKE_NONCOPYABLE(FontResourceCache);

public:
    FontResourceCache();
    FontResourceCache(FontResourceCache&&);
    FontResourceCache& operator=(FontResourceCache&&);
    ~FontResourceCache();

    ErrorOr<void> register_font(SurfaceID surface_id, ResourceID font_resource_id, ReadonlyBytes typeface_bytes, u32 ttc_index);
    ErrorOr<void> register_local_font(SurfaceID surface_id, ResourceID font_resource_id, ReadonlyBytes payload_bytes);
    void unregister_font(SurfaceID surface_id, ResourceID font_resource_id);
    void clear_surface(SurfaceID surface_id);
    SkTypeface* lookup(SurfaceID surface_id, ResourceID font_resource_id) const;
    void clear();

private:
    struct Impl;
    NonnullOwnPtr<Impl> m_impl;
};

}
