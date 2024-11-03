/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibIDL/Types.h>
#include <LibJS/Runtime/VM.h>

namespace Web::WebIDL {

struct ResolvedOverload {
    // Corresponds to "the special value “missing”" in the overload resolution algorithm.
    struct Missing { };
    using Argument = Variant<JS::Value, Missing>;

    int callable_id;
    Vector<Argument> arguments;
};

// https://webidl.spec.whatwg.org/#es-overloads
JS::ThrowCompletionOr<ResolvedOverload> resolve_overload(JS::VM&, IDL::EffectiveOverloadSet&, ReadonlySpan<StringView> interface_dictionaries);

}
