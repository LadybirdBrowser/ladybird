/*
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibCore/Timer.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

struct AnimationFrameCallbackDriver {
    using Callback = Function<void(double)>;

    [[nodiscard]] WebIDL::UnsignedLong add(Callback handler)
    {
        auto id = ++m_animation_frame_callback_identifier;
        m_callbacks.set(id, move(handler));
        return id;
    }

    bool remove(WebIDL::UnsignedLong id)
    {
        auto it = m_callbacks.find(id);
        if (it == m_callbacks.end())
            return false;
        m_callbacks.remove(it);
        return true;
    }

    void run(double now)
    {
        auto taken_callbacks = move(m_callbacks);
        for (auto& [id, callback] : taken_callbacks)
            callback(now);
    }

    bool has_callbacks() const
    {
        return !m_callbacks.is_empty();
    }

private:
    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#animation-frame-callback-identifier
    WebIDL::UnsignedLong m_animation_frame_callback_identifier { 0 };

    OrderedHashMap<WebIDL::UnsignedLong, Callback> m_callbacks;
};

}
