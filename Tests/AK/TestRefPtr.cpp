/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>

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

TEST_CASE(basics)
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

TEST_CASE(assign_reference)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
    object = *object;
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(assign_ptr)
{
    RefPtr<Object> object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
    object = object.ptr();
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(copy_move_ref)
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

TEST_CASE(swap)
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

TEST_CASE(assign_moved_self)
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

TEST_CASE(assign_copy_self)
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

TEST_CASE(self_observers)
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

TEST_CASE(adopt_ref_if_nonnull)
{
    RefPtr<SelfAwareObject> object = adopt_ref_if_nonnull(new (nothrow) SelfAwareObject);
    EXPECT_EQ(object.is_null(), false);
    EXPECT_EQ(object->ref_count(), 1u);

    SelfAwareObject* null_object = nullptr;
    RefPtr<SelfAwareObject> failed_allocation = adopt_ref_if_nonnull(null_object);
    EXPECT_EQ(failed_allocation.is_null(), true);
}

TEST_CASE(destroy_self_owning_refcounted_object)
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
