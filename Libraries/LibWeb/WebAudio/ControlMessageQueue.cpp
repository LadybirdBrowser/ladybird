/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/Debug.h>
namespace Web::WebAudio {

ControlMessageQueue::~ControlMessageQueue()
{
    // Best-effort cleanup for any remaining enqueued messages.
    auto* nodes = m_head.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    while (nodes) {
        auto* next = nodes->next;
        delete nodes;
        nodes = next;
    }
}

void ControlMessageQueue::set_wake_callback(Function<void()> callback)
{
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker(m_wake_callback_mutex);
    m_wake_callback = move(callback);
}

void ControlMessageQueue::enqueue(ControlMessage message)
{
    ASSERT_CONTROL_THREAD();
    auto* node = new (nothrow) Node { .message = move(message), .next = nullptr };
    if (!node)
        return;

    // Push onto an MPSC stack.
    auto* expected = m_head.load(AK::MemoryOrder::memory_order_acquire);
    do {
        node->next = expected;
    } while (!m_head.compare_exchange_strong(expected, node, AK::MemoryOrder::memory_order_release));

    {
        Threading::MutexLocker locker(m_wake_callback_mutex);
        if (m_wake_callback)
            m_wake_callback();
    }
}

Vector<ControlMessage> ControlMessageQueue::drain()
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // "rendering a render quantum", step 2: process the control message queue.

    // Spec-aligned: atomically swap the queue with an empty one.
    auto* nodes = m_head.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    Vector<ControlMessage> messages;
    while (nodes) {
        auto* next = nodes->next;
        messages.append(move(nodes->message));
        delete nodes;
        nodes = next;
    }

    // Enqueue happens LIFO; restore FIFO order.
    messages.reverse();
    return messages;
}

}
