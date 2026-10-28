/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

struct wl_display;
struct wl_event_queue;
struct wl_interface;
struct wl_registry;

namespace Ladybird {

// The globals of the Wayland compositor that Qt is connected to. They are listed on an event queue of our own, as are
// the objects bound from them, so that Qt's queue never sees their events.
class WaylandRegistry {
    AK_MAKE_NONCOPYABLE(WaylandRegistry);
    AK_MAKE_NONMOVABLE(WaylandRegistry);

public:
    AK_ALLOC_WITH_KMALLOC;

    // Off Wayland, there is no registry.
    static OwnPtr<WaylandRegistry> create();
    ~WaylandRegistry();

    // Null if the compositor lacks the global, or has only an older version of it.
    template<typename T>
    T* bind(wl_interface const& interface, u32 version) { return static_cast<T*>(bind_proxy(interface, version)); }

    // Waits until the compositor has sent every event for our queue that it sends in response to what we asked so far.
    void roundtrip();

    // An object that outlives the registry has to be dispatched by Qt, on the main thread.
    static void move_to_qt_queue(void* proxy);

private:
    explicit WaylandRegistry(wl_display*);

    void* bind_proxy(wl_interface const&, u32 version);

    struct Global {
        u32 name { 0 };
        ByteString interface;
        u32 version { 0 };
    };

    wl_display* m_display { nullptr };
    wl_event_queue* m_queue { nullptr };
    wl_display* m_display_on_queue { nullptr };
    wl_registry* m_registry { nullptr };
    Vector<Global> m_globals;
};

}
