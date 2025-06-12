/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/OwnPtr.h>

static u64 deleter_call_count = 0;

TEST_CASE(should_call_custom_deleter)
{
    auto deleter = [](auto* p) { if (p) ++deleter_call_count; };
    auto ptr = OwnPtr<u64, decltype(deleter)> {};
    ptr.clear();
    EXPECT_EQ(0u, deleter_call_count);
    ptr = adopt_own_if_nonnull(&deleter_call_count);
    EXPECT_EQ(0u, deleter_call_count);
    ptr.clear();
    EXPECT_EQ(1u, deleter_call_count);
}

TEST_CASE(destroy_self_owning_object)
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
