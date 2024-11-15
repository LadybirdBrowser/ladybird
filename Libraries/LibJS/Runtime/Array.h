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
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

class Array : public Object {
    JS_OBJECT(Array, Object);
    GC_DECLARE_ALLOCATOR(Array);

public:
    static ThrowCompletionOr<GC::Ref<Array>> create(Realm&, u64 length, Object* prototype = nullptr);
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
        auto values = GC::MarkedVector<Value> { realm.heap() };
        values.ensure_capacity(elements.size());
        for (auto const& element : elements)
            values.append(map_fn(element));

        return Array::create_from(realm, values);
    }

    virtual ~Array() override = default;

    virtual ThrowCompletionOr<Optional<PropertyDescriptor>> internal_get_own_property(PropertyKey const&) const override final;
    virtual ThrowCompletionOr<bool> internal_define_own_property(PropertyKey const&, PropertyDescriptor const&, Optional<PropertyDescriptor>* precomputed_get_own_property = nullptr) override final;
    virtual ThrowCompletionOr<bool> internal_delete(PropertyKey const&) override;
    virtual ThrowCompletionOr<GC::MarkedVector<Value>> internal_own_property_keys() const override final;

    [[nodiscard]] bool length_is_writable() const { return m_length_writable; }

protected:
    explicit Array(Object& prototype);

private:
    ThrowCompletionOr<bool> set_length(PropertyDescriptor const&);

    bool m_length_writable { true };
};

enum class Holes {
    SkipHoles,
    ReadThroughHoles,
};

ThrowCompletionOr<GC::MarkedVector<Value>> sort_indexed_properties(VM&, Object const&, size_t length, Function<ThrowCompletionOr<double>(Value, Value)> const& sort_compare, Holes holes);
ThrowCompletionOr<double> compare_array_elements(VM&, Value x, Value y, FunctionObject* comparefn);

}
