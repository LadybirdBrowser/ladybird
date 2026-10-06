/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// Boolean.prototype is a BooleanObject too, as in the C++ runtime.
class JS_API BooleanObject : public Object {
public:
    static GC::Ref<BooleanObject> create(Realm&, bool);

    bool boolean() const;

    static bool is_engine_class_of(Object const& object) { return object.is_of_engine_class_or_subclass(JS_LAYOUT_CLASS_ID_BOOLEAN_OBJECT); }
};

}
