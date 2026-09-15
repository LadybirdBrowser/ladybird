/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class MapPrototype final : public PrototypeObject<MapPrototype, Map> {
    JS_PROTOTYPE_OBJECT(MapPrototype, Map, Map);
    GC_DECLARE_ALLOCATOR(MapPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~MapPrototype() override = default;

    // The original Map.prototype.entries, which is also the original Map.prototype[%Symbol.iterator%].
    FunctionObject const& entries_function() const { return *m_entries_function; }

private:
    explicit MapPrototype(Realm&);

    virtual void visit_edges(Visitor&) override;

    GC::Ptr<FunctionObject> m_entries_function;

    JS_DECLARE_NATIVE_FUNCTION(clear);
    JS_DECLARE_NATIVE_FUNCTION(delete_);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(get);
    JS_DECLARE_NATIVE_FUNCTION(get_or_insert);
    JS_DECLARE_NATIVE_FUNCTION(get_or_insert_computed);
    JS_DECLARE_NATIVE_FUNCTION(has);
    JS_DECLARE_NATIVE_FUNCTION(keys);
    JS_DECLARE_NATIVE_FUNCTION(set);
    JS_DECLARE_NATIVE_FUNCTION(values);

    JS_DECLARE_NATIVE_FUNCTION(size_getter);
};

}
