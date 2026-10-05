/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// Unlike the JavaScript methods, which turn a -0 into +0, the operations compare keys with SameValue.
class JS_API Map : public Object {
public:
    static GC::Ref<Map> create(Realm&);

    void map_clear();
    bool map_remove(Value const&);
    void map_set(Value const&, Value);
    size_t map_size() const;

    // Calls the callback with the key and value of every entry, in insertion order, and stops at the first error
    // the callback returns. The callback may modify the map; this visits entries the way Map.prototype.forEach does.
    ThrowCompletionOr<void> for_each_entry(Function<ThrowCompletionOr<void>(Value key, Value value)> const&) const;

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_MAP; }
};

}
