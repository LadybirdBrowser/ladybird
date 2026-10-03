/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/HTML/EventLoop/FrameCompletion.h>

namespace Web::HTML {

FrameCompletion& FrameCompletion::the()
{
    // The frame's thread may post after the main thread has begun exiting, so this is never destroyed.
    static NeverDestroyed<FrameCompletion> s_the;
    return *s_the;
}

void FrameCompletion::register_event_loop(Function<void()> deliver)
{
    MutexLocker locker(m_mutex);
    m_deliver = move(deliver);
    m_event_loop = Core::EventLoop::current_weak();
}

void FrameCompletion::post()
{
    MutexLocker locker(m_mutex);
    // A post while a delivery is queued is seen by that delivery.
    if (m_delivery_queued || !m_event_loop)
        return;
    auto event_loop = m_event_loop->take();
    if (!event_loop.is_alive())
        return;
    m_delivery_queued = true;
    event_loop->deferred_invoke([this] { deliver(); });
    // A post from another thread does not wake the loop by itself.
    event_loop->wake();
}

void FrameCompletion::deliver()
{
    {
        MutexLocker locker(m_mutex);
        m_delivery_queued = false;
    }
    // Outside the lock: the steps may take the frame in, which may post another completion.
    if (m_deliver)
        m_deliver();
}

}
