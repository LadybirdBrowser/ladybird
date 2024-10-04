/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/WeakPtr.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>

namespace Core {

ChildEvent::ChildEvent(Type type, EventReceiver& child, EventReceiver* insertion_before_child)
    : Core::Event(type)
    , m_child(child.make_weak_ptr())
    , m_insertion_before_child(AK::make_weak_ptr_if_nonnull(insertion_before_child))
{
}

EventReceiver* ChildEvent::child()
{
    if (auto ref = m_child.strong_ref())
        return ref.ptr();
    return nullptr;
}

EventReceiver const* ChildEvent::child() const
{
    if (auto ref = m_child.strong_ref())
        return ref.ptr();
    return nullptr;
}

EventReceiver* ChildEvent::insertion_before_child()
{
    if (auto ref = m_insertion_before_child.strong_ref())
        return ref.ptr();
    return nullptr;
}

EventReceiver const* ChildEvent::insertion_before_child() const
{
    if (auto ref = m_insertion_before_child.strong_ref())
        return ref.ptr();
    return nullptr;
}

}
