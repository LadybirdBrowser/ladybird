/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <LibSync/Mutex.h>
#include <LibSync/Weakable.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

namespace {

struct Target
    : public AtomicRefCounted<Target>
    , public Sync::Weakable<Target> {
    int value { 42 };
};

struct PlainTarget : public Sync::Weakable<PlainTarget> {
    using Sync::Weakable<PlainTarget>::revoke_weak_refs;
    int value { 7 };
};

}

TEST_CASE(strong_ref_while_alive)
{
    auto target = adopt_ref(*new Target);
    auto weak = target->make_weak_ref();

    auto strong = weak.strong_ref();
    EXPECT(strong);
    EXPECT_EQ(strong->value, 42);
}

TEST_CASE(strong_ref_after_destruction_is_null)
{
    Sync::WeakRef<Target> weak;
    {
        auto target = adopt_ref(*new Target);
        weak = target->make_weak_ref();
        EXPECT(weak.strong_ref());
    }
    EXPECT(!weak.strong_ref());
}

// with_target() needs no reference counting from the target, only revocation before destruction.
TEST_CASE(with_target_works_without_ref_counting)
{
    int observed = 0;
    Sync::WeakRef<PlainTarget> weak;
    {
        PlainTarget target;
        weak = target.make_weak_ref();
        weak.with_target([&](PlainTarget& target) { observed = target.value; });
        EXPECT_EQ(observed, 7);
    }
    weak.with_target([&](PlainTarget&) { observed = -1; });
    EXPECT_EQ(observed, 7);
}

TEST_CASE(with_target_does_not_run_after_explicit_revocation)
{
    PlainTarget target;
    auto weak = target.make_weak_ref();
    target.revoke_weak_refs();

    bool ran = false;
    weak.with_target([&](PlainTarget&) { ran = true; });
    EXPECT(!ran);
}

// A reader thread races strong_ref() against the owner thread destroying targets: every strong_ref
// must hand back a valid ref or null, never a dangling one.
TEST_CASE(strong_ref_races_destruction_safely)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stop { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::Mutex weak_mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::WeakRef<Target> shared_weak;

    auto reader = Threading::Thread::construct("WeakReader"sv, [&]() {
        while (!stop.load(AK::MemoryOrder::memory_order_relaxed)) {
            Sync::WeakRef<Target> weak;
            {
                Sync::MutexLocker locker { weak_mutex };
                weak = shared_weak;
            }
            if (auto strong = weak.strong_ref())
                EXPECT_EQ(strong->value, 42);
        }
        return 0;
    });
    reader->start();

    for (int i = 0; i < 200000; i++) {
        auto target = adopt_ref(*new Target);
        {
            Sync::MutexLocker locker { weak_mutex };
            shared_weak = target->make_weak_ref();
        }
        // target is dropped and destroyed here while the reader may be mid-strong_ref().
    }

    stop.store(true, AK::MemoryOrder::memory_order_relaxed);
    (void)reader->join();
}

// A reader thread races with_target() against the owner thread revoking: revocation must wait out
// in-flight callbacks, so no callback may ever observe the poisoned post-revocation value.
TEST_CASE(with_target_races_revocation_safely)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> stop { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::Mutex weak_mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::WeakRef<PlainTarget> shared_weak;

    auto reader = Threading::Thread::construct("WithTarget"sv, [&]() {
        while (!stop.load(AK::MemoryOrder::memory_order_relaxed)) {
            Sync::WeakRef<PlainTarget> weak;
            {
                Sync::MutexLocker locker { weak_mutex };
                weak = shared_weak;
            }
            weak.with_target([](PlainTarget& target) {
                EXPECT_EQ(target.value, 7);
            });
        }
        return 0;
    });
    reader->start();

    for (int i = 0; i < 200000; i++) {
        PlainTarget target;
        {
            Sync::MutexLocker locker { weak_mutex };
            shared_weak = target.make_weak_ref();
        }
        target.revoke_weak_refs();
        // No callback can be in flight past this point, so none may see the poisoned value.
        target.value = -1;
    }

    stop.store(true, AK::MemoryOrder::memory_order_relaxed);
    (void)reader->join();
}
