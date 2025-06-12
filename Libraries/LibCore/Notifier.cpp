/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>

namespace Core {

Notifier::Notifier(int fd, Type type, EventReceiver* parent)
    : EventReceiver(parent)
    , m_fd(fd)
    , m_type(type)
{
    set_enabled(true);
}

Notifier::~Notifier()
{
    set_enabled(false);
}

void Notifier::set_enabled(bool enabled)
{
    if (m_fd < 0)
        return;
    if (enabled == m_is_enabled)
        return;
    m_is_enabled = enabled;
    if (enabled)
        Core::EventLoop::register_notifier({}, *this);
    else
        Core::EventLoop::unregister_notifier({}, *this);
}

void Notifier::close()
{
    if (m_fd < 0)
        return;
    set_enabled(false);
    m_fd = -1;
}

void Notifier::set_type(Type type)
{
    if (m_is_enabled) {
        // FIXME: Directly communicate intent to the EventLoop.
        set_enabled(false);
        m_type = type;
        set_enabled(true);
    } else {
        m_type = type;
    }
}

void Notifier::event(Core::Event& event)
{
    if (event.type() == Core::Event::NotifierActivation) {
        if (on_activation)
            on_activation();
        return;
    }
    EventReceiver::event(event);
}

}
