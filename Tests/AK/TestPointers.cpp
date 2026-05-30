/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/WeakPtr.h>

TEST_CASE(own_ptr_should_call_custom_deleter)
{
    static u64 deleter_call_count = 0;
    auto deleter = [](auto* p) { if (p) ++deleter_call_count; };
    auto ptr = OwnPtr<u64, decltype(deleter)> {};
    ptr.clear();
    EXPECT_EQ(0u, deleter_call_count);
    ptr = adopt_own_if_nonnull(&deleter_call_count);
    EXPECT_EQ(0u, deleter_call_count);
    ptr.clear();
    EXPECT_EQ(1u, deleter_call_count);
}

TEST_CASE(own_ptr_destroy_self_owning_object)
{
    struct SelfOwning {
        OwnPtr<SelfOwning> self;
    };
    OwnPtr<SelfOwning> object = make<SelfOwning>();
    auto* object_ptr = object.ptr();
    object->self = move(object);
    object = nullptr;
    object_ptr->self = nullptr;
}

TEST_CASE(nonnull_own_ptr_destroy_self_owning_object)
{
    // This test is a little convoluted because SelfOwning can't own itself
    // through a NonnullOwnPtr directly. We have to use an intermediate object ("Inner").
    struct SelfOwning {
        SelfOwning()
        {
        }
        struct Inner {
            explicit Inner(NonnullOwnPtr<SelfOwning> self)
                : self(move(self))
            {
            }
            NonnullOwnPtr<SelfOwning> self;
        };
        OwnPtr<Inner> inner;
    };
    OwnPtr<SelfOwning> object = make<SelfOwning>();
    auto* object_ptr = object.ptr();
    object_ptr->inner = make<SelfOwning::Inner>(object.release_nonnull());
    object_ptr->inner = nullptr;
}

struct Foo { };

template<>
struct AK::Formatter<Foo> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Foo const&)
    {
        return Formatter<StringView>::format(builder, ":^)"sv);
    }
};

TEST_CASE(nonnull_own_ptr_formatter)
{
    auto foo = make<Foo>();
    EXPECT_EQ(MUST(String::formatted("{}", foo)), ":^)"sv);
}

struct Object : public RefCounted<Object> {
    int x;
};

struct Object2 : Object {
};

struct SelfAwareObject : public RefCounted<SelfAwareObject> {
    void will_be_destroyed() { ++num_destroyed; }

    static size_t num_destroyed;
};
size_t SelfAwareObject::num_destroyed = 0;

TEST_CASE(ref_ptr_basics)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT(object.ptr() != nullptr);
    EXPECT_EQ(object->ref_count(), 1u);
    object->ref();
    EXPECT_EQ(object->ref_count(), 2u);
    object->unref();
    EXPECT_EQ(object->ref_count(), 1u);

    {
        NonnullRefPtr another = *object;
        EXPECT_EQ(object->ref_count(), 2u);
    }

    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(ref_ptr_assign_reference)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
    object = *object;
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(ref_ptr_assign_ptr)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
    object = object.ptr();
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(ref_ptr_copy_move_ref)
{
    RefPtr<Object2> object = adopt_ref(*new Object2);
    EXPECT_EQ(object->ref_count(), 1u);
    {
        auto object2 = object;
        EXPECT_EQ(object->ref_count(), 2u);

        RefPtr<Object> object1 = object;
        EXPECT_EQ(object->ref_count(), 3u);

        object1 = move(object2);
        EXPECT_EQ(object->ref_count(), 2u);

        RefPtr<Object> object3(move(object1));
        EXPECT_EQ(object3->ref_count(), 2u);

        object1 = object3;
        EXPECT_EQ(object3->ref_count(), 3u);
    }
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(ref_ptr_swap)
{
    RefPtr<Object> object_a = adopt_ref(*new Object);
    RefPtr<Object> object_b = adopt_ref(*new Object);
    auto* ptr_a = object_a.ptr();
    auto* ptr_b = object_b.ptr();
    swap(object_a, object_b);
    EXPECT_EQ(object_a, ptr_b);
    EXPECT_EQ(object_b, ptr_a);
    EXPECT_EQ(object_a->ref_count(), 1u);
    EXPECT_EQ(object_b->ref_count(), 1u);
}

TEST_CASE(ref_ptr_assign_moved_self)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wself-move"
    object = move(object);
#pragma GCC diagnostic pop
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(ref_ptr_assign_copy_self)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);

#if defined(AK_COMPILER_CLANG)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wself-assign-overloaded"
#endif
    object = object;
#if defined(AK_COMPILER_CLANG)
#    pragma clang diagnostic pop
#endif

    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(ref_ptr_self_observers)
{
    {
        RefPtr<SelfAwareObject> object = adopt_ref(*new SelfAwareObject);
        EXPECT_EQ(object->ref_count(), 1u);
        EXPECT_EQ(SelfAwareObject::num_destroyed, 0u);

        object->ref();
        EXPECT_EQ(object->ref_count(), 2u);
        EXPECT_EQ(SelfAwareObject::num_destroyed, 0u);

        object->unref();
        EXPECT_EQ(object->ref_count(), 1u);
        EXPECT_EQ(SelfAwareObject::num_destroyed, 0u);
    }
    EXPECT_EQ(SelfAwareObject::num_destroyed, 1u);
}

TEST_CASE(ref_ptr_adopt_ref_if_nonnull)
{
    RefPtr<SelfAwareObject> object = adopt_ref_if_nonnull(new (nothrow) SelfAwareObject);
    EXPECT_EQ(object.is_null(), false);
    EXPECT_EQ(object->ref_count(), 1u);

    SelfAwareObject* null_object = nullptr;
    RefPtr<SelfAwareObject> failed_allocation = adopt_ref_if_nonnull(null_object);
    EXPECT_EQ(failed_allocation.is_null(), true);
}

TEST_CASE(ref_ptr_destroy_self_owning_refcounted_object)
{
    struct SelfOwningRefCounted : public RefCounted<SelfOwningRefCounted> {
        RefPtr<SelfOwningRefCounted> self;
    };
    RefPtr object = make_ref_counted<SelfOwningRefCounted>();
    auto* object_ptr = object.ptr();
    object->self = object;
    object = nullptr;
    object_ptr->self = nullptr;
}

TEST_CASE(nonnull_ref_ptr_destroy_self_owning_object)
{
    // This test is a little convoluted because SelfOwning can't own itself
    // through a NonnullOwnPtr directly. We have to use an intermediate object ("Inner").
    struct SelfOwning {
        SelfOwning()
        {
        }
        struct Inner {
            explicit Inner(NonnullOwnPtr<SelfOwning> self)
                : self(move(self))
            {
            }
            NonnullOwnPtr<SelfOwning> self;
        };
        OwnPtr<Inner> inner;
    };
    OwnPtr<SelfOwning> object = make<SelfOwning>();
    auto* object_ptr = object.ptr();
    object_ptr->inner = make<SelfOwning::Inner>(object.release_nonnull());
    object_ptr->inner = nullptr;
}

TEST_CASE(nonnull_ref_ptr_formatter)
{
    auto foo = make<Foo>();
    EXPECT_EQ(MUST(String::formatted("{}", foo)), ":^)"sv);
}

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

TEST_CASE(weak_ptr_basic_weak)
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

TEST_CASE(weak_ptr_weakptr_move)
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

TEST_CASE(weak_ptr_weak_callback_ref_counted)
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

TEST_CASE(weak_ptr_weak_callback_ref_counted_dead)
{
    auto cb = [&] {
        auto simple = adopt_ref(*new SimpleWeakable);
        return weak_callback(*simple, [](auto&) {
            VERIFY_NOT_REACHED();
        });
    }();

    cb();
}

TEST_CASE(weak_ptr_weak_callback_ref_counted_with_args)
{
    int received_value = 0;

    auto simple = adopt_ref(*new SimpleWeakable);
    auto cb = weak_callback(*simple, [&received_value](auto& self, int value) {
        received_value = value + self.member();
    });

    cb(42);
    EXPECT_EQ(received_value, 42 + 123);
}

TEST_CASE(weak_ptr_weak_callback_non_ref_counted)
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

TEST_CASE(weak_ptr_weak_callback_non_ref_counted_dead)
{
    auto cb = [&] {
        NonRefCountedWeakable obj;
        return weak_callback(obj, [](auto&) {
            VERIFY_NOT_REACHED();
        });
    }();

    cb();
}
