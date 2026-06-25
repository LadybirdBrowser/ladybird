/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SharedCircularQueue.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

using TestQueue = Core::SharedSingleProducerCircularQueue<int>;
using QueueError = ErrorOr<int, TestQueue::QueueStatus>;

// There is one parallel consumer, but nobody is producing at the same time.
TEST_CASE(simple_multithread)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto queue = MUST(TestQueue::create());
    auto const test_count = 10;

    for (int i = 0; i < test_count; ++i)
        (void)queue.enqueue(i);

    auto second_thread = Threading::Thread::construct("QueueConsumer"sv, [&queue]() {
        auto copied_queue = queue;
        for (int i = 0; i < test_count; ++i) {
            QueueError result = TestQueue::QueueStatus::Invalid;
            do {
                result = copied_queue.dequeue();
                if (!result.is_error())
                    EXPECT_EQ(result.value(), i);
            } while (result.is_error() && result.error() == TestQueue::QueueStatus::Empty);

            if (result.is_error())
                FAIL("Unexpected error while dequeueing.");
        }
        return 0;
    });
    second_thread->start();
    (void)second_thread->join();

    EXPECT_EQ(queue.weak_used(), (size_t)0);
}

// There is one parallel consumer and one parallel producer.
TEST_CASE(producer_consumer_multithread)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto queue = MUST(TestQueue::create());
    // Ensure that we have the possibility of filling the queue up.
    auto const test_count = queue.size() * 4;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> other_thread_running { false };

    auto second_thread = Threading::Thread::construct("QueueConsumer"sv, [&queue, &other_thread_running]() {
        auto copied_queue = queue;
        other_thread_running.store(true);
        for (size_t i = 0; i < test_count; ++i) {
            QueueError result = TestQueue::QueueStatus::Invalid;
            do {
                result = copied_queue.dequeue();
                if (!result.is_error())
                    EXPECT_EQ(result.value(), (int)i);
            } while (result.is_error() && result.error() == TestQueue::QueueStatus::Empty);

            if (result.is_error())
                FAIL("Unexpected error while dequeueing.");
        }
        return 0;
    });
    second_thread->start();

    while (!other_thread_running.load())
        ;

    for (size_t i = 0; i < test_count; ++i) {
        ErrorOr<void, TestQueue::QueueStatus> result = TestQueue::QueueStatus::Invalid;
        do {
            result = queue.enqueue((int)i);
        } while (result.is_error() && result.error() == TestQueue::QueueStatus::Full);

        if (result.is_error())
            FAIL("Unexpected error while enqueueing.");
    }

    (void)second_thread->join();

    EXPECT_EQ(queue.weak_used(), (size_t)0);
}
