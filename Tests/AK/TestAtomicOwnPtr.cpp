/*
 * Copyright (c) 2024, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/AtomicOwnPtr.h>

static u64 deleter_call_count = 0;

TEST_CASE(should_call_custom_deleter)
{
    auto deleter = [](auto* p) { if (p) ++deleter_call_count; };
    auto ptr = AtomicOwnPtr<u64, decltype(deleter)> {};
    ptr.clear();
    EXPECT_EQ(0u, deleter_call_count);
    ptr = adopt_atomic_own_if_nonnull(&deleter_call_count);
    EXPECT_EQ(0u, deleter_call_count);
    ptr.clear();
    EXPECT_EQ(1u, deleter_call_count);
}

TEST_CASE(destroy_self_owning_object)
{
    struct SelfOwning {
        AtomicOwnPtr<SelfOwning> self;
    };
    AtomicOwnPtr<SelfOwning> object = make<SelfOwning>();
    auto* object_ptr = object.ptr();
    object->self = move(object);
    object = nullptr;
    object_ptr->self = nullptr;
}
