/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SharedCircularQueue.h>
#include <LibCore/System.h>
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

// The counters live in memory the peer can write, so they may claim more items than the queue holds.
TEST_CASE(counters_claiming_more_than_the_queue_holds_are_refused)
{
    auto producer = MUST(TestQueue::create());
    auto consumer = MUST(TestQueue::create(MUST(Core::System::dup(producer.fd()))));
    auto peer_view = MUST(Core::AnonymousBuffer::create_from_anon_fd(MUST(Core::System::dup(producer.fd())), sizeof(AK::SPCQData<int, 32>)));
    auto& counters = *reinterpret_cast<AK::SPCQData<int, 32>*>(peer_view.data<void>());

    MUST(producer.enqueue(1));
    counters.m_tail.store(counters.m_head.load() + (1ull << 40));
    EXPECT(!consumer.peek().has_value());
    EXPECT(!consumer.peek_newest().has_value());
    auto dequeued = consumer.dequeue();
    EXPECT(dequeued.is_error());
    EXPECT_EQ(dequeued.error(), TestQueue::QueueStatus::Invalid);
    EXPECT(consumer.has_seen_corrupt_counters());
    EXPECT(TestQueue::create(MUST(Core::System::dup(producer.fd()))).is_error());

    // A head that has overtaken the tail is just as impossible, and must not let the producer write.
    counters.m_tail.store(1);
    counters.m_head.store(2);
    auto enqueued = producer.enqueue(2);
    EXPECT(enqueued.is_error());
    EXPECT_EQ(enqueued.error(), TestQueue::QueueStatus::Invalid);
    EXPECT(!producer.can_enqueue());
}
