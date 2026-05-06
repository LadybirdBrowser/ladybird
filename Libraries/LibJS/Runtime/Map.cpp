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
    auto old_external_memory_size = external_memory_size();
    m_keys.clear();
    m_entries.clear();
    account_external_memory_change(old_external_memory_size);
}

// 24.1.3.3 Map.prototype.delete ( key ), https://tc39.es/ecma262/#sec-map.prototype.delete
bool Map::map_remove(Value const& key)
{
    Optional<size_t> index;

    for (auto it = m_keys.begin(); !it.is_end(); ++it) {
        if (ValueTraits::equals(*it, key)) {
            index = it.key();
            break;
        }
    }

    if (!index.has_value())
        return false;

    auto old_external_memory_size = external_memory_size();
    m_keys.remove(*index);
    m_entries.remove(key);
    account_external_memory_change(old_external_memory_size);
    return true;
}

// 24.1.3.6 Map.prototype.get ( key ), https://tc39.es/ecma262/#sec-map.prototype.get
Optional<Value> Map::map_get(Value const& key) const
{
    if (auto it = m_entries.find(key); it != m_entries.end())
        return it->value;
    return {};
}

// 24.1.3.7 Map.prototype.has ( key ), https://tc39.es/ecma262/#sec-map.prototype.has
bool Map::map_has(Value const& key) const
{
    return m_entries.contains(key);
}

// 24.1.3.9 Map.prototype.set ( key, value ), https://tc39.es/ecma262/#sec-map.prototype.set
void Map::map_set(Value const& key, Value value)
{
    auto it = m_entries.find(key);
    if (it != m_entries.end()) {
        it->value = value;
    } else {
        auto old_external_memory_size = external_memory_size();
        auto index = m_next_insertion_id++;
        m_keys.insert(index, key);
        m_entries.set(key, value);
        account_external_memory_change(old_external_memory_size);
    }
}

size_t Map::map_size() const
{
    return m_keys.size();
}

static size_t map_key_tree_external_memory_size(RedBlackTree<size_t, Value> const& keys)
{
    constexpr auto approximate_node_size = sizeof(AK::BaseRedBlackTree<size_t>::Node) + sizeof(Value);
    if (keys.size() > NumericLimits<size_t>::max() / approximate_node_size)
        return NumericLimits<size_t>::max();
    return keys.size() * approximate_node_size;
}

size_t Map::external_memory_size() const
{
    auto size = Object::external_memory_size();
    size = saturating_add_external_memory_size(size, map_key_tree_external_memory_size(m_keys));
    size = saturating_add_external_memory_size(size, hash_map_external_memory_size(m_entries));
    return size;
}

void Map::account_external_memory_change(size_t old_external_memory_size)
{
    auto new_external_memory_size = external_memory_size();
    if (new_external_memory_size > old_external_memory_size)
        heap().did_allocate_external_memory(new_external_memory_size - old_external_memory_size);
    else if (old_external_memory_size > new_external_memory_size)
        heap().did_free_external_memory(old_external_memory_size - new_external_memory_size);
}

void Map::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& value : m_entries) {
        visitor.visit(value.key);
        visitor.visit(value.value);
    }
    // NOTE: The entries in m_keys are already visited by the walk over m_entries above.
    visitor.ignore(m_keys);
}

}
