/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/Debug.h>

using namespace Web::WebAudio;

TEST_CASE(associated_task_queue_drains_in_fifo_order_on_single_thread)
{
    AssociatedTaskQueue queue;

    Vector<int> executed;

    queue.enqueue([&] {
        executed.append(1);
    });
    queue.enqueue([&] {
        executed.append(2);
    });
    queue.enqueue([&] {
        executed.append(3);
    });

    auto render_thread = Threading::Thread::construct("RenderThread"sv, [&] {
        Web::WebAudio::mark_current_thread_as_render_thread();

        auto tasks = queue.drain();
        EXPECT_EQ(tasks.size(), 3u);

        for (auto& task : tasks)
            task();

        // Draining should empty the queue.
        EXPECT(queue.drain().is_empty());
        return 0;
    });
    render_thread->start();
    (void)TRY_OR_FAIL(render_thread->join());

    EXPECT_EQ(executed.size(), 3u);
    if (executed.size() != 3u)
        return;
    EXPECT_EQ(executed[0], 1);
    EXPECT_EQ(executed[1], 2);
    EXPECT_EQ(executed[2], 3);
}

TEST_CASE(associated_task_queue_is_thread_safe_for_multiple_producers)
{
    AssociatedTaskQueue queue;

    static constexpr size_t producer_count = 4;
    static constexpr size_t tasks_per_producer = 250;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> executed_count { 0 };

    Vector<NonnullRefPtr<Threading::Thread>> producers;
    producers.ensure_capacity(producer_count);

    for (size_t producer_index = 0; producer_index < producer_count; ++producer_index) {
        producers.unchecked_append(Threading::Thread::construct("Producer"sv, [&queue, &executed_count] {
            for (size_t i = 0; i < tasks_per_producer; ++i) {
                queue.enqueue([&executed_count] {
                    executed_count.fetch_add(1);
                });
            }
            return 0;
        }));
    }

    for (auto& thread : producers)
        thread->start();
    for (auto& thread : producers)
        (void)TRY_OR_FAIL(thread->join());

    auto render_thread = Threading::Thread::construct("RenderThread"sv, [&] {
        Web::WebAudio::mark_current_thread_as_render_thread();

        auto tasks = queue.drain();
        EXPECT_EQ(tasks.size(), producer_count * tasks_per_producer);

        for (auto& task : tasks)
            task();

        EXPECT(queue.drain().is_empty());
        return 0;
    });
    render_thread->start();
    (void)TRY_OR_FAIL(render_thread->join());

    EXPECT_EQ(executed_count.load(), static_cast<u32>(producer_count * tasks_per_producer));
}
