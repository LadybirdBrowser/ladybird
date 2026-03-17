/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibJS/Runtime/Accessor.h>
#include <LibJS/Runtime/IndexedProperties.h>

namespace JS {

bool GenericIndexedPropertyStorage::has_index(u32 index) const
{
    return m_sparse_elements.contains(index);
}

Optional<ValueAndAttributes> GenericIndexedPropertyStorage::get(u32 index) const
{
    if (index >= m_array_size)
        return {};
    return m_sparse_elements.get(index).copy();
}

void GenericIndexedPropertyStorage::put(u32 index, Value value, PropertyAttributes attributes)
{
    if (index >= m_array_size)
        m_array_size = index + 1;
    m_sparse_elements.set(index, { value, attributes });
}

void GenericIndexedPropertyStorage::remove(u32 index)
{
    VERIFY(index < m_array_size);
    m_sparse_elements.remove(index);
}

ValueAndAttributes GenericIndexedPropertyStorage::take_first()
{
    VERIFY(m_array_size > 0);
    m_array_size--;

    auto indices = m_sparse_elements.keys();
    quick_sort(indices);

    auto it = m_sparse_elements.find(indices.first());
    auto first_element = it->value;
    m_sparse_elements.remove(it);
    return first_element;
}

ValueAndAttributes GenericIndexedPropertyStorage::take_last()
{
    VERIFY(m_array_size > 0);
    m_array_size--;

    auto result = m_sparse_elements.get(m_array_size);
    if (!result.has_value())
        return {};
    m_sparse_elements.remove(m_array_size);
    return result.value();
}

bool GenericIndexedPropertyStorage::set_array_like_size(size_t new_size)
{
    if (new_size == m_array_size)
        return true;

    if (new_size >= m_array_size) {
        m_array_size = new_size;
        return true;
    }

    bool any_failed = false;
    size_t highest_index = 0;

    HashMap<u32, ValueAndAttributes> new_sparse_elements;
    for (auto& entry : m_sparse_elements) {
        if (entry.key >= new_size) {
            if (entry.value.attributes.is_configurable())
                continue;
            else
                any_failed = true;
        }
        new_sparse_elements.set(entry.key, entry.value);
        highest_index = max(highest_index, entry.key);
    }

    if (any_failed)
        m_array_size = highest_index + 1;
    else
        m_array_size = new_size;

    m_sparse_elements = move(new_sparse_elements);
    return !any_failed;
}

}
