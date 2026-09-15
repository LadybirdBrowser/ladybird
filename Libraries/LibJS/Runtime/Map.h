/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibJS/Export.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueTraits.h>

namespace JS {

class JS_API Map : public Object {
    JS_OBJECT(Map, Object);
    GC_DECLARE_ALLOCATOR(Map);

    struct StoredEntry {
        Value key;
        Value value;
        u64 insertion_id { 0 };

        bool is_removed() const { return key.is_special_empty_value(); }
    };

public:
    static GC::Ref<Map> create(Realm&);

    virtual ~Map() override = default;

    virtual bool is_map_object() const final { return true; }

    void map_clear();
    bool map_remove(Value const&);
    Optional<Value> map_get(Value const&) const;
    bool map_has(Value const&) const;
    void map_set(Value const&, Value);
    size_t map_size() const { return m_indices.size(); }

    virtual size_t external_memory_size() const override;

    // Calls the callback with the key and value of every entry, in insertion order.
    // The callback must not modify the map.
    template<typename Callback>
    void for_each_entry(Callback callback) const
    {
        [[maybe_unused]] auto generation = m_generation;
        [[maybe_unused]] auto entry_count = m_entries.size();
        for (auto const& entry : m_entries) {
            if (!entry.is_removed())
                callback(entry.key, entry.value);
        }
        VERIFY(generation == m_generation && entry_count == m_entries.size());
    }

    struct EndIterator {
    };

    struct Entry {
        Value key;
        Value value;
    };

    // An iterator that stays valid while the map is modified, with the visiting rules of the spec's index-based loops:
    // entries added during iteration are visited, removed entries are skipped, and entries that were moved by a
    // compaction or cleared are found again by their insertion ID.
    template<bool IsConst>
    struct IteratorImpl {
        bool is_end() const
        {
            find_current_entry();
            return m_index >= m_map->m_entries.size();
        }

        // NB: This moves past the entry that is_end() or operator*() found last, even if that entry has been removed
        //     since, so removing the current entry during iteration does not skip the entry after it.
        IteratorImpl& operator++()
        {
            if (!m_current_insertion_id.has_value()) {
                find_current_entry();
                if (!m_current_insertion_id.has_value())
                    return *this;
            }
            m_next_insertion_id = *m_current_insertion_id + 1;
            m_current_insertion_id.clear();
            auto const& entries = m_map->m_entries;
            if (m_generation == m_map->m_generation && m_index < entries.size() && entries[m_index].insertion_id < m_next_insertion_id)
                ++m_index;
            return *this;
        }

        Entry operator*() const
        {
            find_current_entry();
            auto const& entry = m_map->m_entries[m_index];
            return { entry.key, entry.value };
        }

        bool operator==(IteratorImpl const& other) const { return m_map.ptr() == other.m_map.ptr() && m_next_insertion_id == other.m_next_insertion_id; }
        bool operator==(EndIterator const&) const { return is_end(); }

        void visit_edges(Cell::Visitor& visitor)
        {
            visitor.visit(m_map);
        }

    private:
        friend class Map;
        IteratorImpl(Map const& map)
        requires(IsConst)
            : m_map(map)
            , m_generation(map.m_generation)
        {
        }

        IteratorImpl(Map& map)
        requires(!IsConst)
            : m_map(map)
            , m_generation(map.m_generation)
        {
        }

        void find_current_entry() const
        {
            auto const& entries = m_map->m_entries;
            if (m_generation != m_map->m_generation) [[unlikely]] {
                m_index = m_map->index_of_first_entry_not_inserted_before(m_next_insertion_id);
                m_generation = m_map->m_generation;
            }
            while (m_index < entries.size() && entries[m_index].is_removed())
                ++m_index;
            if (m_index < entries.size())
                m_current_insertion_id = entries[m_index].insertion_id;
            else
                m_current_insertion_id.clear();
        }

        Conditional<IsConst, GC::Ref<Map const>, GC::Ref<Map>> m_map;

        // The position of the current entry in m_entries. Only meaningful while m_generation matches the map.
        mutable size_t m_index { 0 };
        mutable u64 m_generation { 0 };

        // Every entry with a smaller insertion ID has already been visited or skipped.
        u64 m_next_insertion_id { 0 };
        mutable Optional<u64> m_current_insertion_id;
    };

    using Iterator = IteratorImpl<false>;
    using ConstIterator = IteratorImpl<true>;

    ConstIterator begin() const { return { *this }; }
    Iterator begin() { return { *this }; }
    EndIterator end() const { return {}; }

private:
    explicit Map(Object& prototype);
    virtual void visit_edges(Visitor& visitor) override;

    size_t index_of_first_entry_not_inserted_before(u64 insertion_id) const;
    void compact_entries();

    size_t storage_external_memory_size() const;
    void account_storage_external_memory_change(size_t old_storage_external_memory_size);

    // All entries in insertion order. Removed entries stay in place (as holes) until the next compaction, so the
    // positions held by iterators stay valid. Compacting or clearing moves entries, which bumps m_generation.
    Vector<StoredEntry> m_entries;
    HashMap<Value, size_t, ValueTraits> m_indices;
    size_t m_removed_entry_count { 0 };
    u64 m_next_insertion_id { 0 };
    u64 m_generation { 0 };
};

template<>
inline bool Object::fast_is<Map>() const { return is_map_object(); }

}
