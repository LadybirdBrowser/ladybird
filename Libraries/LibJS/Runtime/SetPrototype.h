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
    GC_DECLARE_ALLOCATOR(SetPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SetPrototype() override = default;

    // The original Set.prototype.values, which is also the original Set.prototype.keys and
    // Set.prototype[%Symbol.iterator%].
    FunctionObject const& values_function() const { return *m_values_function; }

private:
    explicit SetPrototype(Realm&);

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<FunctionObject> m_values_function;

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
