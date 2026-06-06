/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/WebIDL/OverloadTypes.h>

namespace Web::WebIDL {

ParameterizedType const& Type::as_parameterized() const
{
    return as<ParameterizedType const>(*this);
}

ParameterizedType& Type::as_parameterized()
{
    return as<ParameterizedType>(*this);
}

UnionType const& Type::as_union() const
{
    return as<UnionType const>(*this);
}

UnionType& Type::as_union()
{
    return as<UnionType>(*this);
}

bool Type::includes_nullable_type() const
{
    if (is_nullable())
        return true;

    if (is_union() && as_union().number_of_nullable_member_types() == 1)
        return true;

    return false;
}

bool Type::includes_undefined() const
{
    if (is_undefined())
        return true;

    if (is_union())
        return as_union().member_types().contains([](auto& type) { return type->includes_undefined(); });

    return false;
}

bool Type::is_buffer() const
{
    return m_name.is_one_of("ArrayBuffer", "SharedArrayBuffer");
}

bool Type::is_typed_array() const
{
    return m_name.is_one_of("Int8Array", "Int16Array", "Int32Array", "Uint8Array", "Uint16Array", "Uint32Array", "Uint8ClampedArray", "BigInt64Array", "BigUint64Array", "Float16Array", "Float32Array", "Float64Array");
}

bool Type::is_buffer_view() const
{
    return m_name == "DataView" || is_typed_array();
}

bool Type::is_buffer_source() const
{
    return is_buffer() || is_buffer_view();
}

Vector<NonnullRefPtr<Type const>> UnionType::flattened_member_types() const
{
    Vector<NonnullRefPtr<Type const>> types;
    for (auto& type : m_member_types) {
        if (type->is_union())
            types.extend(type->as_union().flattened_member_types());
        else
            types.append(type);
    }
    return types;
}

size_t UnionType::number_of_nullable_member_types() const
{
    size_t num_nullable_member_types = 0;
    for (auto& type : m_member_types) {
        if (type->is_nullable())
            ++num_nullable_member_types;

        if (type->is_union())
            num_nullable_member_types += type->as_union().number_of_nullable_member_types();
    }
    return num_nullable_member_types;
}

void EffectiveOverloadSet::remove_all_other_entries()
{
    Vector<Item> new_items;
    new_items.append(m_items[*m_last_matching_item_index]);
    m_items = move(new_items);
}

}
