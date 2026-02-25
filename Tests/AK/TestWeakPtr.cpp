/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>

#if defined(AK_COMPILER_CLANG)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wunused-private-field"
#endif

class SimpleWeakable : public Weakable<SimpleWeakable>
    , public RefCounted<SimpleWeakable> {
public:
    SimpleWeakable() = default;

    int member() const { return m_member; }

private:
    int m_member { 123 };
};

class NonRefCountedWeakable : public Weakable<NonRefCountedWeakable> {
public:
    NonRefCountedWeakable() = default;

    int value() const { return m_value; }

private:
    int m_value { 456 };
};

#if defined(AK_COMPILER_CLANG)
#    pragma clang diagnostic pop
#endif

TEST_CASE(basic_weak)
{
    WeakPtr<SimpleWeakable> weak1;
    WeakPtr<SimpleWeakable> weak2;

    {
        auto simple = adopt_ref(*new SimpleWeakable);
        weak1 = simple;
        weak2 = simple;
        EXPECT_EQ(weak1.is_null(), false);
        EXPECT_EQ(weak2.is_null(), false);
        EXPECT_EQ(weak1.strong_ref().ptr(), simple.ptr());
        EXPECT_EQ(weak1.strong_ref().ptr(), weak2.strong_ref().ptr());
    }

    EXPECT_EQ(weak1.is_null(), true);
    EXPECT_EQ(weak1.strong_ref().ptr(), nullptr);
    EXPECT_EQ(weak1.strong_ref().ptr(), weak2.strong_ref().ptr());
}

TEST_CASE(weakptr_move)
{
    WeakPtr<SimpleWeakable> weak1;
    WeakPtr<SimpleWeakable> weak2;

    {
        auto simple = adopt_ref(*new SimpleWeakable);
        weak1 = simple;
        weak2 = move(weak1);
        EXPECT_EQ(weak1.is_null(), true);
        EXPECT_EQ(weak2.is_null(), false);
        EXPECT_EQ(weak2.strong_ref().ptr(), simple.ptr());
    }

    EXPECT_EQ(weak2.is_null(), true);
}

TEST_CASE(weak_callback_ref_counted)
{
    bool was_called = false;

    {
        auto simple = adopt_ref(*new SimpleWeakable);
        auto cb = weak_callback(*simple, [&was_called](auto& self) {
            was_called = true;
            EXPECT_EQ(self.member(), 123);
        });

        cb();
        EXPECT(was_called);
    }
}

TEST_CASE(weak_callback_ref_counted_dead)
{
    auto cb = [&] {
        auto simple = adopt_ref(*new SimpleWeakable);
        return weak_callback(*simple, [](auto&) {
            VERIFY_NOT_REACHED();
        });
    }();

    cb();
}

TEST_CASE(weak_callback_ref_counted_with_args)
{
    int received_value = 0;

    auto simple = adopt_ref(*new SimpleWeakable);
    auto cb = weak_callback(*simple, [&received_value](auto& self, int value) {
        received_value = value + self.member();
    });

    cb(42);
    EXPECT_EQ(received_value, 42 + 123);
}

TEST_CASE(weak_callback_non_ref_counted)
{
    bool was_called = false;

    {
        NonRefCountedWeakable obj;
        auto cb = weak_callback(obj, [&was_called](auto& self) {
            was_called = true;
            EXPECT_EQ(self.value(), 456);
        });

        cb();
        EXPECT(was_called);
    }
}

TEST_CASE(weak_callback_non_ref_counted_dead)
{
    auto cb = [&] {
        NonRefCountedWeakable obj;
        return weak_callback(obj, [](auto&) {
            VERIFY_NOT_REACHED();
        });
    }();

    cb();
}
