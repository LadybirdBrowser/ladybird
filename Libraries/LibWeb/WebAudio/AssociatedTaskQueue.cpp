/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/Debug.h>

namespace Web::WebAudio {

AssociatedTaskQueue::~AssociatedTaskQueue()
{
    auto* nodes = m_head.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    while (nodes) {
        auto* next = nodes->next;
        delete nodes;
        nodes = next;
    }
}

void AssociatedTaskQueue::set_wake_callback(Function<void()> callback)
{
    ASSERT_CONTROL_THREAD();
    Threading::MutexLocker locker(m_wake_callback_mutex);
    m_wake_callback = move(callback);
}

void AssociatedTaskQueue::enqueue(Task task)
{
    ASSERT_CONTROL_THREAD();
    auto* node = new (nothrow) Node { .task = move(task), .next = nullptr };
    if (!node)
        return;

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

Vector<AssociatedTaskQueue::Task> AssociatedTaskQueue::drain()
{
    ASSERT_RENDER_THREAD();
    auto* nodes = m_head.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    Vector<Task> tasks;
    while (nodes) {
        auto* next = nodes->next;
        tasks.append(move(nodes->task));
        delete nodes;
        nodes = next;
    }
    tasks.reverse();
    return tasks;
}

}
