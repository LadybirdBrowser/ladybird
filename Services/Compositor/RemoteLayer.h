/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
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

    // Shows the part of the buffer that the frame was painted into, at the top left of the layer.
    void show_frame(Gfx::SharedImageBuffer const&, Gfx::IntSize painted_size, double device_pixel_ratio);

private:
    RemoteLayer() = default;

#ifdef AK_OS_MACOS
    void* m_context { nullptr };
    void* m_root_layer { nullptr };
    void* m_content_layer { nullptr };
#endif
    u32 m_core_animation_context_id { 0 };
};

#ifndef AK_OS_MACOS
inline OwnPtr<RemoteLayer> RemoteLayer::try_create()
{
    return nullptr;
}

inline RemoteLayer::~RemoteLayer() = default;

inline void RemoteLayer::show_frame(Gfx::SharedImageBuffer const&, Gfx::IntSize, double)
{
}
#endif

}
