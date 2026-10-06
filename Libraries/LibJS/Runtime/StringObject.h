/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// String.prototype is a StringObject too, as in the C++ runtime.
class JS_API StringObject : public Object {
public:
    [[nodiscard]] static GC::Ref<StringObject> create(Realm&, PrimitiveString&, Object& prototype);

    PrimitiveString const& primitive_string() const;
    PrimitiveString& primitive_string();

    static bool is_engine_class_of(Object const& object) { return object.is_of_engine_class_or_subclass(JS_LAYOUT_CLASS_ID_STRING_OBJECT); }
};

}
