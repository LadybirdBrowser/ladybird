/*
 * Copyright (c) 2025, Trey Shaffer <trey@trsh.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ControlMessageQueue);

GC::Ref<ControlMessageQueue> ControlMessageQueue::create(JS::Realm& realm)
{
    return realm.create<ControlMessageQueue>(realm);
}

ControlMessageQueue::ControlMessageQueue(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

ControlMessageQueue::~ControlMessageQueue() = default;

void ControlMessageQueue::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
}

void ControlMessageQueue::enqueue(GC::Ref<GC::Function<void()>> message)
{
    Threading::MutexLocker locker(m_mutex);
    m_messages.append(move(message));

    // FIXME: Signal the rendering thread when implemented
}

bool ControlMessageQueue::has_messages() const
{
    Threading::MutexLocker locker(m_mutex);
    return !m_messages.is_empty();
}

void ControlMessageQueue::process_messages()
{
    while (true) {
        Vector<GC::Ref<GC::Function<void()>>> messages_to_process;

        {
            Threading::MutexLocker locker(m_mutex);

            if (m_is_processing)
                return;

            if (m_messages.is_empty())
                return;

            m_is_processing = true;

            // Snapshot prevents deadlock from reentrant enqueue
            messages_to_process = move(m_messages);
            m_messages.clear();
        }

        // FIXME: Should be called from the rendering thread

        for (auto& message : messages_to_process) {
            message->function()();
        }

        {
            Threading::MutexLocker locker(m_mutex);
            if (m_messages.is_empty()) {
                m_is_processing = false;
                return;
            }
        }
    }
}

void ControlMessageQueue::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    Threading::MutexLocker locker(m_mutex);
    for (auto& message : m_messages)
        visitor.visit(message);
}

}
