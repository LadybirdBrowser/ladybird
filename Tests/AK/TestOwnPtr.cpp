/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/OwnPtr.h>

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
