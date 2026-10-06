/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

// An Array exotic object, or an object of a class that extends Array, such as a host array.
class JS_API Array : public Object {
public:
    static ThrowCompletionOr<GC::Ref<Array>> create(Realm&, u64 length, GC::Ptr<Object> prototype = nullptr);
    static GC::Ref<Array> create_from(Realm&, ReadonlySpan<Value>);

    template<size_t N>
    static GC::Ref<Array> create_from(Realm& realm, Value const (&values)[N])
    {
        return create_from(realm, ReadonlySpan<Value> { values, N });
    }

    // Non-standard but equivalent to CreateArrayFromList.
    template<typename T>
    static GC::Ref<Array> create_from(Realm& realm, ReadonlySpan<T> elements, Function<Value(T const&)> map_fn)
    {
        GC::RootVector<Value> values;
        values.ensure_capacity(elements.size());
        for (auto const& element : elements)
            values.append(map_fn(element));

        return Array::create_from(realm, values);
    }

    // The runtime gives exactly the Array exotic objects, host arrays included, the magical length property.
    static bool is_engine_class_of(Object const& object) { return object.has_magical_length_property(); }
};

}
