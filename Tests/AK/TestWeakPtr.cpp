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

private:
    int m_member { 123 };
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
