/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Set.h>

namespace JS {

class SetPrototype final : public PrototypeObject<SetPrototype, Set> {
    JS_PROTOTYPE_OBJECT(SetPrototype, Set, Set);
    JS_DECLARE_ALLOCATOR(SetPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SetPrototype() override = default;

private:
    explicit SetPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(add);
    JS_DECLARE_NATIVE_FUNCTION(clear);
    JS_DECLARE_NATIVE_FUNCTION(delete_);
    JS_DECLARE_NATIVE_FUNCTION(difference);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(has);
    JS_DECLARE_NATIVE_FUNCTION(intersection);
    JS_DECLARE_NATIVE_FUNCTION(is_disjoint_from);
    JS_DECLARE_NATIVE_FUNCTION(is_subset_of);
    JS_DECLARE_NATIVE_FUNCTION(is_superset_of);
    JS_DECLARE_NATIVE_FUNCTION(size_getter);
    JS_DECLARE_NATIVE_FUNCTION(symmetric_difference);
    JS_DECLARE_NATIVE_FUNCTION(union_);
    JS_DECLARE_NATIVE_FUNCTION(values);
};

}
