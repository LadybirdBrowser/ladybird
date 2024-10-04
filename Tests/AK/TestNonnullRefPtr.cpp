/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>

struct Object : public RefCounted<Object> {
    int x;
};

TEST_CASE(basics)
{
    auto object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
    object->ref();
    EXPECT_EQ(object->ref_count(), 2u);
    object->unref();
    EXPECT_EQ(object->ref_count(), 1u);

    {
        NonnullRefPtr another = object;
        EXPECT_EQ(object->ref_count(), 2u);
    }

    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(assign_reference)
{
    auto object = adopt_ref(*new Object);
    EXPECT_EQ(object->ref_count(), 1u);
    object = *object;
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(assign_owner_of_self)
{
    struct Object : public RefCounted<Object> {
        RefPtr<Object> parent;
    };

    auto parent = adopt_ref(*new Object);
    auto child = adopt_ref(*new Object);
    child->parent = move(parent);

    child = *child->parent;
    EXPECT_EQ(child->ref_count(), 1u);
}

TEST_CASE(swap_with_self)
{
    auto object = adopt_ref(*new Object);
    swap(object, object);
    EXPECT_EQ(object->ref_count(), 1u);
}

TEST_CASE(destroy_self_owning_refcounted_object)
{
    // This test is a little convoluted because SelfOwningRefCounted can't own itself
    // through a NonnullRefPtr directly. We have to use an intermediate object ("Inner").
    struct SelfOwningRefCounted : public RefCounted<SelfOwningRefCounted> {
        SelfOwningRefCounted()
            : inner(make<Inner>(*this))
        {
        }
        struct Inner {
            explicit Inner(SelfOwningRefCounted& self)
                : self(self)
            {
            }
            NonnullRefPtr<SelfOwningRefCounted> self;
        };
        OwnPtr<Inner> inner;
    };
    RefPtr object = make_ref_counted<SelfOwningRefCounted>();
    auto* object_ptr = object.ptr();
    object = nullptr;
    object_ptr->inner = nullptr;
}
