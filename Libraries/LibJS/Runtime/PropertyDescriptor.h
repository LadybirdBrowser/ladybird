/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

// 6.2.5 The Property Descriptor Specification Type, https://tc39.es/ecma262/#sec-property-descriptor-specification-type

Value from_property_descriptor(VM&, Optional<PropertyDescriptor> const&);
ThrowCompletionOr<PropertyDescriptor> to_property_descriptor(VM&, Value);

class PropertyDescriptor {
public:
    [[nodiscard]] bool is_accessor_descriptor() const;
    [[nodiscard]] bool is_data_descriptor() const;
    [[nodiscard]] bool is_generic_descriptor() const;

    [[nodiscard]] PropertyAttributes attributes() const;

    void complete();

    // Not a standard abstract operation, but "If every field in Desc is absent".
    [[nodiscard]] bool is_empty() const
    {
        return !value.has_value() && !get.has_value() && !set.has_value() && !writable.has_value() && !enumerable.has_value() && !configurable.has_value() && !unimplemented.has_value();
    }

    Optional<Value> value {};
    Optional<GCPtr<FunctionObject>> get {};
    Optional<GCPtr<FunctionObject>> set {};
    Optional<bool> writable {};
    Optional<bool> enumerable {};
    Optional<bool> configurable {};
    Optional<bool> unimplemented {};

    Optional<u32> property_offset {};
};

}

namespace AK {

template<>
struct Formatter<JS::PropertyDescriptor> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, JS::PropertyDescriptor const& property_descriptor)
    {
        Vector<String> parts;
        if (property_descriptor.value.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Value]]: {}", property_descriptor.value->to_string_without_side_effects()))));
        if (property_descriptor.get.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Get]]: JS::Function* @ {:p}", property_descriptor.get->ptr()))));
        if (property_descriptor.set.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Set]]: JS::Function* @ {:p}", property_descriptor.set->ptr()))));
        if (property_descriptor.writable.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Writable]]: {}", *property_descriptor.writable))));
        if (property_descriptor.enumerable.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Enumerable]]: {}", *property_descriptor.enumerable))));
        if (property_descriptor.configurable.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Configurable]]: {}", *property_descriptor.configurable))));
        if (property_descriptor.unimplemented.has_value())
            TRY(parts.try_append(TRY(String::formatted("[[Unimplemented]]: {}", *property_descriptor.unimplemented))));
        return Formatter<StringView>::format(builder, TRY(String::formatted("PropertyDescriptor {{ {} }}", TRY(String::join(", "sv, parts)))));
    }
};

}
