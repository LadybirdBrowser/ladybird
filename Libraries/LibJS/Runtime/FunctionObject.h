/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyKey.h>

namespace JS {

class JS_API FunctionObject : public Object {
public:
    // The runtime flags every object that has a [[Call]] internal method, including callable proxies.
    static bool is_engine_class_of(Object const& object) { return object.has_engine_object_flag(JS_LAYOUT_OBJECT_FLAG_IS_FUNCTION); }

    // [[Realm]], which a bound function or a proxy does not have.
    Realm* realm() const;

    Utf16String name_for_call_stack() const;
};

}
