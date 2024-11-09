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
    JS_DECLARE_ALLOCATOR(MapPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~MapPrototype() override = default;

private:
    explicit MapPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(clear);
    JS_DECLARE_NATIVE_FUNCTION(delete_);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(get);
    JS_DECLARE_NATIVE_FUNCTION(has);
    JS_DECLARE_NATIVE_FUNCTION(keys);
    JS_DECLARE_NATIVE_FUNCTION(set);
    JS_DECLARE_NATIVE_FUNCTION(values);

    JS_DECLARE_NATIVE_FUNCTION(size_getter);
};

}
