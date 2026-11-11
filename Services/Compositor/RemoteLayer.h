/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Compositor {

// A CoreAnimation layer tree that this process commits frames into, and that another process shows in a view of its
// own by the ID of the tree's context. Frames then reach the screen without passing through that process.
class RemoteLayer {
    AK_MAKE_NONCOPYABLE(RemoteLayer);
    AK_MAKE_NONMOVABLE(RemoteLayer);

public:
    AK_ALLOC_WITH_KMALLOC;

    // None where the platform offers no such layer trees.
    static OwnPtr<RemoteLayer> try_create();
    ~RemoteLayer();

    u32 core_animation_context_id() const { return m_core_animation_context_id; }

    // Shows the part of the buffer that the frame was painted into, at the top left of the layer at its own size, or
    // scaled to fit within the size of the view given, in device pixels.
    void show_frame(Gfx::SharedImageBuffer const&, Gfx::IntSize painted_size, double device_pixel_ratio, Optional<Gfx::IntSize> fit_within);
    // Places the frame on display again, as when the view that it is scaled into changes size.
    void place_frame(Optional<Gfx::IntSize> fit_within);

    // While held, what every remote layer of this process shows changes nowhere on screen, and changes all at once
    // when released.
    static void hold_changes_on_screen();
    static void release_changes_held_off_screen();

private:
    RemoteLayer() = default;

#ifdef AK_OS_MACOS
    void lay_out_frame(Optional<Gfx::IntSize> fit_within);

    void* m_context { nullptr };
    void* m_root_layer { nullptr };
    void* m_content_layer { nullptr };
    Gfx::IntSize m_painted_size;
    double m_device_pixel_ratio { 1.0 };
#endif
    u32 m_core_animation_context_id { 0 };
};

#ifndef AK_OS_MACOS
inline OwnPtr<RemoteLayer> RemoteLayer::try_create()
{
    return nullptr;
}

inline RemoteLayer::~RemoteLayer() = default;

inline void RemoteLayer::show_frame(Gfx::SharedImageBuffer const&, Gfx::IntSize, double, Optional<Gfx::IntSize>)
{
}

inline void RemoteLayer::place_frame(Optional<Gfx::IntSize>)
{
}

inline void RemoteLayer::hold_changes_on_screen()
{
}

inline void RemoteLayer::release_changes_held_off_screen()
{
}
#endif

}
