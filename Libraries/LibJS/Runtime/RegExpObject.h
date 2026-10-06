/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

JS_API ThrowCompletionOr<GC::Ref<RegExpObject>> regexp_create(VM&, Value pattern, Value flags);

class JS_API RegExpObject : public Object {
public:
    // The runtime keeps [[OriginalSource]] and [[OriginalFlags]] in its own strings, so these return copies.
    Utf16String pattern() const;
    Utf16String flags() const;

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_REG_EXP_OBJECT; }
};

}
