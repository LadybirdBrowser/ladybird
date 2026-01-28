/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <initializer_list>

namespace AK {

// A map datastructure, mapping keys K to values V, based on a hash table with closed hashing.
// HashMap can optionally provide ordered iteration based on the order of keys when IsOrdered = true.
// HashMap is based on HashTable, which should be used instead if just a set datastructure is required.
template<typename K, typename V, typename KeyTraits, typename ValueTraits, bool IsOrdered>
class HashMap {
private:
    struct Entry {
        K key;
        V value;
    };

    struct EntryTraits {
        static constexpr bool may_have_slow_equality_check() { return KeyTraits::may_have_slow_equality_check(); }
        static unsigned hash(Entry const& entry) { return KeyTraits::hash(entry.key); }
        static bool equals(Entry const& a, Entry const& b) { return KeyTraits::equals(a.key, b.key); }
    };

public:
    using KeyType = K;
    using ValueType = V;

    HashMap() = default;

    HashMap(std::initializer_list<Entry> list)
    {
        MUST(try_ensure_capacity(list.size()));
        for (auto& [key, value] : list)
            set(key, value);
    }

    HashMap(HashMap const&) = default; // FIXME: Not OOM-safe! Use clone() instead.
    HashMap(HashMap&& other) noexcept = default;
    HashMap& operator=(HashMap const& other) = default; // FIXME: Not OOM-safe! Use clone() instead.
    HashMap& operator=(HashMap&& other) noexcept = default;

    [[nodiscard]] bool is_empty() const
    {
        return m_table.is_empty();
    }
    [[nodiscard]] size_t size() const { return m_table.size(); }
    [[nodiscard]] size_t capacity() const { return m_table.capacity(); }
    void clear() { m_table.clear(); }
    void clear_with_capacity() { m_table.clear_with_capacity(); }

    HashSetResult set(K const& key, V const& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace) { return m_table.set({ key, value }, existing_entry_behavior); }
    HashSetResult set(K const& key, V&& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace) { return m_table.set({ key, move(value) }, existing_entry_behavior); }
    HashSetResult set(K&& key, V&& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace) { return m_table.set({ move(key), move(value) }, existing_entry_behavior); }
    ErrorOr<HashSetResult> try_set(K const& key, V const& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace) { return m_table.try_set({ key, value }, existing_entry_behavior); }
    ErrorOr<HashSetResult> try_set(K const& key, V&& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace) { return m_table.try_set({ key, move(value) }, existing_entry_behavior); }
    ErrorOr<HashSetResult> try_set(K&& key, V&& value, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Replace) { return m_table.try_set({ move(key), move(value) }, existing_entry_behavior); }

    void update(HashMap const& other)
    {
        for (auto const& [key, value] : other)
            set(key, value);
    }

    bool remove(K const& key)
    {
        auto it = find(key);
        if (it != end()) {
            m_table.remove(it);
            return true;
        }
        return false;
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) bool remove(Key const& key)
    {
        auto it = find(key);
        if (it != end()) {
            m_table.remove(it);
            return true;
        }
        return false;
    }

    template<typename TUnaryPredicate>
    bool remove_all_matching(TUnaryPredicate const& predicate)
    {
        return m_table.remove_all_matching([&](auto& entry) {
            return predicate(entry.key, entry.value);
        });
    }

    template<typename TUnaryPredicate>
    Vector<Entry> take_all_matching(TUnaryPredicate const& predicate)
    {
        return m_table.take_all_matching([&](auto& entry) {
            return predicate(entry.key, entry.value);
        });
    }

    using HashTableType = HashTable<Entry, EntryTraits, IsOrdered>;
    using IteratorType = typename HashTableType::Iterator;
    using ConstIteratorType = typename HashTableType::ConstIterator;

    [[nodiscard]] IteratorType begin() { return m_table.begin(); }
    [[nodiscard]] IteratorType end() { return m_table.end(); }
    [[nodiscard]] IteratorType find(K const& key)
    {
        if (m_table.is_empty())
            return m_table.end();
        return m_table.find(KeyTraits::hash(key), [&](auto& entry) { return KeyTraits::equals(entry.key, key); });
    }
    template<typename TUnaryPredicate>
    [[nodiscard]] IteratorType find(unsigned hash, TUnaryPredicate predicate)
    {
        return m_table.find(hash, predicate);
    }

    [[nodiscard]] ConstIteratorType begin() const { return m_table.begin(); }
    [[nodiscard]] ConstIteratorType end() const { return m_table.end(); }
    [[nodiscard]] ConstIteratorType find(K const& key) const
    {
        if (m_table.is_empty())
            return m_table.end();
        return m_table.find(KeyTraits::hash(key), [&](auto& entry) { return KeyTraits::equals(entry.key, key); });
    }
    template<typename TUnaryPredicate>
    [[nodiscard]] ConstIteratorType find(unsigned hash, TUnaryPredicate predicate) const
    {
        return m_table.find(hash, predicate);
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) [[nodiscard]] IteratorType find(Key const& key)
    {
        if (m_table.is_empty())
            return m_table.end();
        return m_table.find(Traits<Key>::hash(key), [&](auto& entry) { return Traits<K>::equals(entry.key, key); });
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) [[nodiscard]] ConstIteratorType find(Key const& key) const
    {
        if (m_table.is_empty())
            return m_table.end();
        return m_table.find(Traits<Key>::hash(key), [&](auto& entry) { return Traits<K>::equals(entry.key, key); });
    }

    ErrorOr<void> try_ensure_capacity(size_t capacity) { return m_table.try_ensure_capacity(capacity); }

    void ensure_capacity(size_t capacity) { return m_table.ensure_capacity(capacity); }

    Optional<typename ValueTraits::ConstPeekType> get(K const& key) const
    requires(!IsPointer<typename ValueTraits::PeekType>)
    {
        auto it = find(key);
        if (it == end())
            return {};
        return (*it).value;
    }

    Optional<typename ValueTraits::ConstPeekType> get(K const& key) const
    requires(IsPointer<typename ValueTraits::PeekType>)
    {
        auto it = find(key);
        if (it == end())
            return {};
        return (*it).value;
    }

    Optional<typename ValueTraits::PeekType> get(K const& key)
    requires(!IsConst<typename ValueTraits::PeekType>)
    {
        auto it = find(key);
        if (it == end())
            return {};
        return (*it).value;
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) Optional<typename ValueTraits::ConstPeekType> get(Key const& key) const
    requires(!IsPointer<typename ValueTraits::PeekType>)
    {
        auto it = find(key);
        if (it == end())
            return {};
        return (*it).value;
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) Optional<typename ValueTraits::ConstPeekType> get(Key const& key) const
    requires(IsPointer<typename ValueTraits::PeekType>)
    {
        auto it = find(key);
        if (it == end())
            return {};
        return (*it).value;
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) Optional<typename ValueTraits::PeekType> get(Key const& key)
    requires(!IsConst<typename ValueTraits::PeekType>)
    {
        auto it = find(key);
        if (it == end())
            return {};
        return (*it).value;
    }

    [[nodiscard]] bool contains(K const& key) const
    {
        return find(key) != end();
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) [[nodiscard]] bool contains(Key const& value) const
    {
        return find(value) != end();
    }

    void remove(IteratorType it)
    {
        m_table.remove(it);
    }

    Optional<V> take(K const& key)
    {
        if (auto it = find(key); it != end()) {
            auto value = move(it->value);
            m_table.remove(it);

            return value;
        }

        return {};
    }

    template<Concepts::HashCompatible<K> Key>
    requires(IsSame<KeyTraits, Traits<K>>) Optional<V> take(Key const& key)
    {
        if (auto it = find(key); it != end()) {
            auto value = move(it->value);
            m_table.remove(it);

            return value;
        }

        return {};
    }

    V take_first()
    requires(IsOrdered)
    {
        VERIFY(!is_empty());
        return take(begin()->key).release_value();
    }

    template<typename Callback>
    V& ensure(K const& key, Callback initialization_callback, HashSetExistingEntryBehavior existing_entry_behavior = HashSetExistingEntryBehavior::Keep)
    {
        return m_table.ensure(KeyTraits::hash(key), [&](auto& entry) { return KeyTraits::equals(entry.key, key); }, [&] -> Entry { return { key, initialization_callback() }; }, existing_entry_behavior).value;
    }

    V& ensure(K const& key)
    {
        return ensure(key, [] { return V(); });
    }

    template<typename Callback>
    ErrorOr<V> try_ensure(K const& key, Callback initialization_callback)
    {
        auto it = find(key);
        if (it != end())
            return it->value;
        if constexpr (FallibleFunction<Callback>) {
            auto result = TRY(try_set(key, TRY(initialization_callback())));
            VERIFY(result == HashSetResult::InsertedNewEntry);
        } else {
            auto result = TRY(try_set(key, initialization_callback()));
            VERIFY(result == HashSetResult::InsertedNewEntry);
        }
        return find(key)->value;
    }

    [[nodiscard]] Vector<K> keys() const
    {
        Vector<K> list;
        list.ensure_capacity(size());
        for (auto const& [key, _] : *this)
            list.unchecked_append(key);
        return list;
    }

    template<typename NewKeyTraits = KeyTraits, typename NewValueTraits = ValueTraits, bool NewIsOrdered = IsOrdered>
    ErrorOr<HashMap<K, V, NewKeyTraits, NewValueTraits, NewIsOrdered>> clone() const
    {
        HashMap<K, V, NewKeyTraits, NewValueTraits, NewIsOrdered> hash_map_clone;
        TRY(hash_map_clone.try_ensure_capacity(size()));
        for (auto const& [key, value] : *this)
            hash_map_clone.set(key, value);
        return hash_map_clone;
    }

    bool operator==(HashMap const& other) const
    {
        if (size() != other.size())
            return false;
        if (is_empty())
            return true;
        for (auto const& [key, value] : *this) {
            auto it = other.find(key);
            if (it == other.end())
                return false;
            if (!ValueTraits::equals(value, it->value))
                return false;
        }
        return true;
    }

    bool operator!=(HashMap const& other) const
    {
        return !(*this == other);
    }

private:
    HashTableType m_table;
};

}

#if USING_AK_GLOBALLY
using AK::HashMap;
using AK::OrderedHashMap;
#endif
