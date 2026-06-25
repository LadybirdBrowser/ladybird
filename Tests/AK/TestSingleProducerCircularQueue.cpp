/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/SingleProducerCircularQueue.h>
#include <LibTest/TestCase.h>

using IntQueue = AK::SingleProducerCircularQueue<int, 4>;

TEST_CASE(fifo_order)
{
    IntQueue queue;
    for (int i = 0; i < 4; ++i)
        MUST(queue.enqueue(i));
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(MUST(queue.dequeue()), i);
}

TEST_CASE(full_and_empty)
{
    IntQueue queue;

    auto empty = queue.dequeue();
    EXPECT(empty.is_error());
    EXPECT_EQ(empty.release_error(), IntQueue::QueueStatus::Empty);

    for (int i = 0; i < 4; ++i)
        MUST(queue.enqueue(i));

    auto full = queue.enqueue(99);
    EXPECT(full.is_error());
    EXPECT_EQ(full.release_error(), IntQueue::QueueStatus::Full);
}

TEST_CASE(enqueue_in_place)
{
    IntQueue queue;
    for (int i = 0; i < 4; ++i)
        MUST(queue.enqueue_in_place([&](int& slot) { slot = i; }));
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(MUST(queue.dequeue()), i);
}

TEST_CASE(peek_does_not_consume)
{
    IntQueue queue;
    EXPECT(!queue.peek().has_value());

    MUST(queue.enqueue(7));
    MUST(queue.enqueue(8));
    EXPECT_EQ(queue.peek().value(), 7);
    EXPECT_EQ(queue.peek().value(), 7);

    EXPECT_EQ(MUST(queue.dequeue()), 7);
    EXPECT_EQ(queue.peek().value(), 8);
}

namespace {

struct Tracked : public RefCounted<Tracked> {
    explicit Tracked(int value)
        : value(value)
    {
        ++s_live;
    }
    ~Tracked() { --s_live; }

    int value { 0 };
    static inline int s_live { 0 };
};

}

// The shared-memory queue forbids non-trivially-copyable elements; the in-process queue must
// construct references on enqueue and destroy them on dequeue and teardown, with no leaks.
TEST_CASE(non_pod_items_are_held_then_moved_out)
{
    EXPECT_EQ(Tracked::s_live, 0);
    {
        AK::SingleProducerCircularQueue<RefPtr<Tracked>, 4> queue;

        auto item = adopt_ref(*new Tracked(7));
        EXPECT_EQ(Tracked::s_live, 1);
        EXPECT_EQ(item->ref_count(), 1u);

        MUST(queue.enqueue(item));
        // Enqueuing takes the queue's own reference into the slot.
        EXPECT_EQ(item->ref_count(), 2u);
        EXPECT_EQ(Tracked::s_live, 1);

        auto dequeued = MUST(queue.dequeue());
        // Dequeue moves the slot's reference out rather than copying it, so the count is unchanged.
        EXPECT_EQ(dequeued->value, 7);
        EXPECT_EQ(item->ref_count(), 2u);
    }
    EXPECT_EQ(Tracked::s_live, 0);
}

TEST_CASE(non_pod_items_are_released_when_the_queue_is_destroyed)
{
    EXPECT_EQ(Tracked::s_live, 0);
    {
        AK::SingleProducerCircularQueue<RefPtr<Tracked>, 4> queue;
        for (int i = 0; i < 3; ++i)
            MUST(queue.enqueue(adopt_ref(*new Tracked(i))));
        EXPECT_EQ(Tracked::s_live, 3);
        // The queue goes out of scope with items still enqueued; its slots must release them.
    }
    EXPECT_EQ(Tracked::s_live, 0);
}

namespace {

struct MoveOnly {
    MoveOnly() = default;
    explicit MoveOnly(int value)
        : value(value)
    {
    }
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
    MoveOnly(MoveOnly const&) = delete;
    MoveOnly& operator=(MoveOnly const&) = delete;

    int value { 0 };
};

}

// A move-only element type is likewise unusable in the shared version; it must round-trip here.
TEST_CASE(move_only_items)
{
    AK::SingleProducerCircularQueue<MoveOnly, 4> queue;
    for (int i = 0; i < 4; ++i)
        MUST(queue.enqueue(MoveOnly(i)));
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(MUST(queue.dequeue()).value, i);
}
