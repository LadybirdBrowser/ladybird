/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/StdLibExtras.h>
#include <LibGC/Cell.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakInlines.h>

namespace GC {

// Cell-typed key and/or value slots are held as Weak<T> so entries vanish
// when their referent is collected; non-cell slots are stored directly.
template<typename K, typename V>
class WeakHashMap {
    static constexpr bool key_is_cell = IsBaseOf<Cell, K>;
    static constexpr bool value_is_cell = IsBaseOf<Cell, V>;

    using KeyStorage = Conditional<key_is_cell, Weak<K>, K>;
    using ValueStorage = Conditional<value_is_cell, Weak<V>, V>;

    struct KeyTraits : public DefaultTraits<KeyStorage> {
        static unsigned hash(KeyStorage const& value)
        {
            if constexpr (key_is_cell)
                return Traits<K const*>::hash(value.ptr());
            else
                return Traits<K>::hash(value);
        }
    };

    using TableType = HashMap<KeyStorage, ValueStorage, KeyTraits>;

public:
    WeakHashMap() = default;

    HashSetResult set(K const& key, V const& value)
    {
        maybe_prune();
        return m_table.set(to_key_storage(key), to_value_storage(value));
    }

    HashSetResult set(K const& key, V&& value)
    {
        maybe_prune();
        return m_table.set(to_key_storage(key), to_value_storage(move(value)));
    }

    bool remove(K const& key)
    {
        maybe_prune();
        auto it = find_iterator(key);
        if (it == m_table.end())
            return false;
        m_table.remove(it);
        return true;
    }

    template<typename Callback>
    V& ensure(K const& key, Callback initialization_callback)
    {
        maybe_prune();

        auto it = find_iterator(key);
        if (it != m_table.end()) {
            if constexpr (value_is_cell) {
                if (auto value = it->value.ptr())
                    return *value;
                m_table.remove(it);
            } else {
                return it->value;
            }
        }

        if constexpr (value_is_cell) {
            auto value = initialization_callback();
            [[maybe_unused]] auto result = m_table.set(to_key_storage(key), to_value_storage(value));
            VERIFY(result == HashSetResult::InsertedNewEntry);
            return *value;
        } else {
            return m_table.ensure(to_key_storage(key), [&] {
                return to_value_storage(initialization_callback());
            });
        }
    }

    V& ensure(K const& key)
    requires(!value_is_cell)
    {
        return ensure(key, [] { return V(); });
    }

    bool contains(K const& key) const
    {
        return find_iterator(key) != m_table.end();
    }

    auto get(K const& key)
    {
        if constexpr (value_is_cell) {
            auto it = find_iterator(key);
            if (it == m_table.end())
                return static_cast<V*>(nullptr);
            return static_cast<V*>(it->value.ptr());
        } else {
            auto it = find_iterator(key);
            if (it == m_table.end())
                return Optional<V&> {};
            return Optional<V&> { it->value };
        }
    }

    auto get(K const& key) const
    {
        if constexpr (value_is_cell) {
            auto it = find_iterator(key);
            if (it == m_table.end())
                return static_cast<V const*>(nullptr);
            return static_cast<V const*>(it->value.ptr());
        } else {
            auto it = find_iterator(key);
            if (it == m_table.end())
                return Optional<V const&> {};
            return Optional<V const&> { it->value };
        }
    }

    bool is_empty() const
    {
        for (auto const& entry : m_table) {
            if constexpr (key_is_cell) {
                if (!entry.key.ptr())
                    continue;
            }
            if constexpr (value_is_cell) {
                if (!entry.value.ptr())
                    continue;
            }
            return false;
        }
        return true;
    }

    void clear() { m_table.clear(); }

private:
    static KeyStorage to_key_storage(K const& key)
    {
        if constexpr (key_is_cell)
            return Weak<K>(key);
        else
            return key;
    }

    template<typename U>
    static ValueStorage to_value_storage(U&& value)
    {
        if constexpr (value_is_cell)
            return Weak<V>(forward<U>(value));
        else
            return ValueStorage { forward<U>(value) };
    }

    auto find_iterator(K const& key) const -> typename TableType::ConstIteratorType
    {
        if constexpr (key_is_cell) {
            return m_table.find(Traits<K const*>::hash(&key), [&](auto& entry) {
                return entry.key.ptr() == &key;
            });
        } else {
            return m_table.find(key);
        }
    }

    auto find_iterator(K const& key) -> typename TableType::IteratorType
    {
        if constexpr (key_is_cell) {
            return m_table.find(Traits<K const*>::hash(&key), [&](auto& entry) {
                return entry.key.ptr() == &key;
            });
        } else {
            return m_table.find(key);
        }
    }

    void maybe_prune()
    {
        if (++m_mutations_since_last_prune < max(m_table.size(), static_cast<size_t>(64)))
            return;
        m_table.remove_all_matching([](auto const& key, auto const& value) {
            if constexpr (key_is_cell) {
                if (!key.ptr())
                    return true;
            }
            if constexpr (value_is_cell) {
                if (!value.ptr())
                    return true;
            }
            return false;
        });
        m_mutations_since_last_prune = 0;
    }

    TableType m_table;
    size_t m_mutations_since_last_prune { 0 };
};

}
