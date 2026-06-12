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

// These first three cases don't multithread at all.

TEST_CASE(simple_enqueue)
{
    auto queue = MUST(TestQueue::create());
    for (size_t i = 0; i < queue.size(); ++i)
        MUST(queue.enqueue((int)i));

    auto result = queue.enqueue(0);
    EXPECT(result.is_error());
    EXPECT_EQ(result.release_error(), TestQueue::QueueStatus::Full);
}

TEST_CASE(enqueue_in_place)
{
    auto queue = MUST(TestQueue::create());
    auto const test_count = 10;
    for (int i = 0; i < test_count; ++i) {
        MUST(queue.enqueue_in_place([&](int& slot) {
            slot = i;
        }));
    }
    for (int i = 0; i < test_count; ++i) {
        auto const element = MUST(queue.dequeue());
        EXPECT_EQ(element, i);
    }
}

TEST_CASE(simple_dequeue)
{
    auto queue = MUST(TestQueue::create());
    auto const test_count = 10;
    for (int i = 0; i < test_count; ++i)
        (void)queue.enqueue(i);
    for (int i = 0; i < test_count; ++i) {
        // TODO: This could be TRY_OR_FAIL(), if someone implements Formatter<SharedSingleProducerCircularQueue::QueueStatus>.
        auto const element = MUST(queue.dequeue());
        EXPECT_EQ(element, i);
    }
}

TEST_CASE(peek_does_not_consume)
{
    auto queue = MUST(TestQueue::create());
    EXPECT(!queue.peek().has_value());

    MUST(queue.enqueue(7));
    MUST(queue.enqueue(8));
    EXPECT_EQ(queue.peek().value(), 7);
    EXPECT_EQ(queue.peek().value(), 7);

    EXPECT_EQ(MUST(queue.dequeue()), 7);
    EXPECT_EQ(queue.peek().value(), 8);
}

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
