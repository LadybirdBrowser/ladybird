/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/Map.h>

namespace JS {

GC_DEFINE_ALLOCATOR(Map);

GC::Ref<Map> Map::create(Realm& realm)
{
    return realm.create<Map>(realm.intrinsics().map_prototype());
}

Map::Map(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 24.1.3.1 Map.prototype.clear ( ), https://tc39.es/ecma262/#sec-map.prototype.clear
void Map::map_clear()
{
    auto old_storage_external_memory_size = storage_external_memory_size();
    m_entries.clear();
    m_indices.clear();
    m_removed_entry_count = 0;
    ++m_generation;
    account_storage_external_memory_change(old_storage_external_memory_size);
}

// 24.1.3.3 Map.prototype.delete ( key ), https://tc39.es/ecma262/#sec-map.prototype.delete
bool Map::map_remove(Value const& key)
{
    auto it = m_indices.find(key);
    if (it == m_indices.end())
        return false;

    auto old_storage_external_memory_size = storage_external_memory_size();

    auto& entry = m_entries[it->value];
    entry.key = js_special_empty_value();
    entry.value = js_undefined();
    m_indices.remove(it);
    ++m_removed_entry_count;

    // Compact once removed entries outnumber the live ones, so removal stays amortized O(1).
    static constexpr size_t minimum_removed_entry_count_for_compaction = 8;
    if (m_removed_entry_count >= minimum_removed_entry_count_for_compaction && m_removed_entry_count > m_indices.size())
        compact_entries();

    account_storage_external_memory_change(old_storage_external_memory_size);
    return true;
}

// 24.1.3.6 Map.prototype.get ( key ), https://tc39.es/ecma262/#sec-map.prototype.get
Optional<Value> Map::map_get(Value const& key) const
{
    if (auto it = m_indices.find(key); it != m_indices.end())
        return m_entries[it->value].value;
    return {};
}

// 24.1.3.7 Map.prototype.has ( key ), https://tc39.es/ecma262/#sec-map.prototype.has
bool Map::map_has(Value const& key) const
{
    return m_indices.contains(key);
}

// 24.1.3.9 Map.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-map.prototype.set
void Map::map_set(Value const& key, Value value)
{
    auto new_index = m_entries.size();
    auto old_storage_external_memory_size = storage_external_memory_size();
    auto index = m_indices.ensure(key, [new_index] { return new_index; });
    if (index != new_index) {
        m_entries[index].value = value;
        return;
    }
    m_entries.append({ key, value, m_next_insertion_id++ });
    account_storage_external_memory_change(old_storage_external_memory_size);
}

size_t Map::index_of_first_entry_not_inserted_before(u64 insertion_id) const
{
    // Entries are stored in insertion order, so their insertion IDs are increasing.
    size_t low = 0;
    size_t high = m_entries.size();
    while (low < high) {
        auto middle = low + (high - low) / 2;
        if (m_entries[middle].insertion_id < insertion_id)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

void Map::compact_entries()
{
    Vector<size_t> new_indices;
    new_indices.resize(m_entries.size());

    size_t live_entry_count = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].is_removed())
            continue;
        new_indices[i] = live_entry_count;
        m_entries[live_entry_count++] = m_entries[i];
    }
    m_entries.shrink(live_entry_count);
    if (m_entries.capacity() > live_entry_count * 2)
        m_entries.shrink_to_fit();

    for (auto& it : m_indices)
        it.value = new_indices[it.value];

    m_removed_entry_count = 0;
    ++m_generation;
}

size_t Map::storage_external_memory_size() const
{
    return saturating_add_external_memory_size(vector_external_memory_size(m_entries), hash_map_external_memory_size(m_indices));
}

size_t Map::external_memory_size() const
{
    return saturating_add_external_memory_size(Object::external_memory_size(), storage_external_memory_size());
}

void Map::account_storage_external_memory_change(size_t old_storage_external_memory_size)
{
    auto new_storage_external_memory_size = storage_external_memory_size();
    if (new_storage_external_memory_size > old_storage_external_memory_size)
        heap().did_allocate_external_memory(new_storage_external_memory_size - old_storage_external_memory_size);
    else if (old_storage_external_memory_size > new_storage_external_memory_size)
        heap().did_free_external_memory(old_storage_external_memory_size - new_storage_external_memory_size);
}

void Map::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& entry : m_entries) {
        visitor.visit(entry.key);
        visitor.visit(entry.value);
    }
    // NOTE: The keys in m_indices are also stored in m_entries, which are visited above.
    visitor.ignore(m_indices);
}

}
