/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashTable.h>
#include <AK/NumericLimits.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace JS {

inline size_t saturating_add_external_memory_size(size_t lhs, size_t rhs)
{
    if (rhs > NumericLimits<size_t>::max() - lhs)
        return NumericLimits<size_t>::max();
    return lhs + rhs;
}

inline size_t string_external_memory_size(String const& string)
{
    if (string.is_short_string())
        return 0;
    return string.byte_count();
}

inline size_t byte_string_external_memory_size(ByteString const& string)
{
    if (string.is_empty())
        return 0;
    return AK::allocation_size_for_stringimpl(string.length());
}

inline size_t utf16_string_external_memory_size(Utf16String const& string)
{
    if (!string.has_long_storage())
        return 0;

    auto code_unit_size = string.has_ascii_storage() ? sizeof(char) : sizeof(char16_t);
    if (string.length_in_code_units() > NumericLimits<size_t>::max() / code_unit_size)
        return NumericLimits<size_t>::max();
    return string.length_in_code_units() * code_unit_size;
}

template<typename T, size_t inline_capacity, FastLastAccess fast_last_access>
size_t vector_external_memory_size(Vector<T, inline_capacity, fast_last_access> const& vector)
{
    if (vector.capacity() <= inline_capacity)
        return 0;
    return vector.capacity() * sizeof(T);
}

template<typename Map>
size_t hash_map_external_memory_size(Map const& map)
{
    return map.capacity() * (sizeof(typename Map::KeyType) + sizeof(typename Map::ValueType));
}

template<typename T, typename TraitsForT, bool is_ordered>
size_t hash_table_external_memory_size(HashTable<T, TraitsForT, is_ordered> const& table)
{
    return table.capacity() * sizeof(T);
}

}
