/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SharedCircularQueue.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <sched.h>

using TestQueue = Core::SharedSingleProducerCircularQueue<int>;
using QueueError = ErrorOr<int, TestQueue::QueueStatus>;

Function<intptr_t()> dequeuer(TestQueue& queue, Atomic<size_t>& dequeue_count, size_t test_count);

// These first two cases don't multithread at all.

TEST_CASE(simple_enqueue)
{
    auto queue = MUST(TestQueue::create());
    for (size_t i = 0; i < queue.size() - 1; ++i)
        MUST(queue.enqueue((int)i));

    auto result = queue.enqueue(0);
    EXPECT(result.is_error());
    EXPECT_EQ(result.release_error(), TestQueue::QueueStatus::Full);
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

// There is one parallel consumer, but nobody is producing at the same time.
TEST_CASE(simple_multithread)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA auto queue = MUST(TestQueue::create());
    auto const test_count = 10;

    for (int i = 0; i < test_count; ++i)
        (void)queue.enqueue(i);

    auto second_thread = Threading::Thread::construct([&queue]() {
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

    auto second_thread = Threading::Thread::construct([&queue, &other_thread_running]() {
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

// There are multiple parallel consumers, but nobody is producing at the same time.
TEST_CASE(multi_consumer)
{
    auto queue = MUST(TestQueue::create());
    // This needs to be divisible by 4!
    size_t const test_count = queue.size() - 4;
    Atomic<size_t> dequeue_count = 0;

    auto threads = {
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
    };

    for (size_t i = 0; i < test_count; ++i)
        (void)queue.enqueue((int)i);

    for (auto thread : threads)
        thread->start();
    for (auto thread : threads)
        (void)thread->join();

    EXPECT_EQ(queue.weak_used(), (size_t)0);
    EXPECT_EQ(dequeue_count.load(), (size_t)test_count);
}

// There are multiple parallel consumers and one parallel producer.
TEST_CASE(single_producer_multi_consumer)
{
    auto queue = MUST(TestQueue::create());
    // Choose a higher number to provoke possible race conditions.
    size_t const test_count = queue.size() * 8;
    Atomic<size_t> dequeue_count = 0;

    auto threads = {
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
        Threading::Thread::construct(dequeuer(queue, dequeue_count, test_count)),
    };
    for (auto thread : threads)
        thread->start();

    for (size_t i = 0; i < test_count; ++i) {
        ErrorOr<void, TestQueue::QueueStatus> result = TestQueue::QueueStatus::Invalid;
        do {
            result = queue.enqueue((int)i);
            // After we put something in the first time, let's wait while nobody has dequeued yet.
            while (dequeue_count.load() == 0)
                ;
            // Give others time to do something.
            sched_yield();
        } while (result.is_error() && result.error() == TestQueue::QueueStatus::Full);

        if (result.is_error())
            FAIL("Unexpected error while enqueueing.");
    }

    for (auto thread : threads)
        (void)thread->join();

    EXPECT_EQ(queue.weak_used(), (size_t)0);
    EXPECT_EQ(dequeue_count.load(), (size_t)test_count);
}

Function<intptr_t()> dequeuer(TestQueue& queue, Atomic<size_t>& dequeue_count, size_t const test_count)
{
    return [&queue, &dequeue_count, test_count]() {
        auto copied_queue = queue;
        for (size_t i = 0; i < test_count / 4; ++i) {
            QueueError result = TestQueue::QueueStatus::Invalid;
            do {
                result = copied_queue.dequeue();
                if (!result.is_error())
                    dequeue_count.fetch_add(1);
                // Give others time to do something.
                sched_yield();
            } while (result.is_error() && result.error() == TestQueue::QueueStatus::Empty);

            if (result.is_error())
                FAIL("Unexpected error while dequeueing.");
        }
        return (intptr_t)0;
    };
}
