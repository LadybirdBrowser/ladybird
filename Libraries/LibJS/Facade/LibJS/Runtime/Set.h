/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// Unlike the JavaScript methods, which turn a -0 into +0, the operations compare values with SameValue.
class JS_API Set : public Object {
public:
    static GC::Ref<Set> create(Realm&);

    void set_clear();
    bool set_remove(Value const& value);
    void set_add(Value const& key);
    size_t set_size() const;

    // Calls the callback with every value, in insertion order, and stops at the first error the callback returns.
    // The callback may modify the set; this visits values the way Set.prototype.forEach does.
    ThrowCompletionOr<void> for_each_value(Function<ThrowCompletionOr<void>(Value)> const&) const;

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_SET; }
};

}
