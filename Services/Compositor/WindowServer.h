/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <Compositor/RemoteLayer.h>

namespace Compositor {

// The window server, as this process reaches it for the views of one presentation.
class WindowServer
    : public RefCounted<WindowServer>
    , public Weakable<WindowServer> {
    AK_MAKE_NONCOPYABLE(WindowServer);
    AK_MAKE_NONMOVABLE(WindowServer);

public:
    AK_ALLOC_WITH_KMALLOC;

    // None where frames reach the screen only through the presentation.
    static RefPtr<WindowServer> try_create_for_presentation();
    virtual ~WindowServer() = default;

    virtual OwnPtr<RemoteLayer> try_create_remote_layer(u64 remote_layer_id) = 0;

    enum class FrameHandOff : u8 {
        OnceGpuWorkIsScheduled,
        OnceGpuWorkIsFinished,
    };
    virtual FrameHandOff frame_hand_off() const = 0;
    static FrameHandOff frame_hand_off_through_presentation();

    // While held, what every remote layer of this window server shows changes nowhere on screen, and changes all at once
    // when released.
    virtual void hold_changes_on_screen() = 0;
    virtual void release_changes_held_off_screen() = 0;

protected:
    WindowServer() = default;
};

}
