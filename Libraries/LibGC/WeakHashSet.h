/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibGC/Weak.h>

namespace GC {

template<typename T>
class WeakHashSet {
    struct InternalTraits : public DefaultTraits<Weak<T>> {
        static unsigned hash(Weak<T> const& value)
        {
            return Traits<T*>::hash(value.ptr());
        }
    };

    using TableType = HashTable<Weak<T>, InternalTraits>;

public:
    WeakHashSet() = default;

    void set(T& value)
    {
        prune_dead_entries();
        m_table.set(Weak<T>(value));
    }

    bool remove(T& value)
    {
        prune_dead_entries();
        auto it = m_table.find(Traits<T*>::hash(&value), [&](auto& entry) { return entry.ptr() == &value; });
        if (it == m_table.end())
            return false;
        m_table.remove(it);
        return true;
    }

    bool contains(T& value) const
    {
        auto it = m_table.find(Traits<T*>::hash(&value), [&](auto& entry) { return entry.ptr() == &value; });
        return it != m_table.end();
    }

    bool is_empty() const
    {
        for (auto const& entry : m_table) {
            if (entry.ptr())
                return false;
        }
        return true;
    }

    void clear() { m_table.clear(); }

    class Iterator {
    public:
        bool operator==(Iterator const& other) const { return m_it == other.m_it; }
        bool operator!=(Iterator const& other) const { return m_it != other.m_it; }

        Iterator& operator++()
        {
            ++m_it;
            skip_dead();
            return *this;
        }

        T& operator*() { return *m_it->ptr(); }
        T* operator->() { return m_it->ptr(); }

    private:
        friend class WeakHashSet;

        using InnerIterator = typename TableType::Iterator;

        explicit Iterator(InnerIterator it, InnerIterator end)
            : m_it(it)
            , m_end(end)
        {
            skip_dead();
        }

        void skip_dead()
        {
            while (m_it != m_end && !m_it->ptr())
                ++m_it;
        }

        InnerIterator m_it;
        InnerIterator m_end;
    };

    class ConstIterator {
    public:
        bool operator==(ConstIterator const& other) const { return m_it == other.m_it; }
        bool operator!=(ConstIterator const& other) const { return m_it != other.m_it; }

        ConstIterator& operator++()
        {
            ++m_it;
            skip_dead();
            return *this;
        }

        T& operator*() { return *(*m_it).ptr(); }
        T* operator->() { return (*m_it).ptr(); }

    private:
        friend class WeakHashSet;

        using InnerIterator = typename TableType::ConstIterator;

        explicit ConstIterator(InnerIterator it, InnerIterator end)
            : m_it(it)
            , m_end(end)
        {
            skip_dead();
        }

        void skip_dead()
        {
            while (m_it != m_end && !m_it->ptr())
                ++m_it;
        }

        InnerIterator m_it;
        InnerIterator m_end;
    };

    Iterator begin() { return Iterator(m_table.begin(), m_table.end()); }
    Iterator end() { return Iterator(m_table.end(), m_table.end()); }

    ConstIterator begin() const { return ConstIterator(m_table.begin(), m_table.end()); }
    ConstIterator end() const { return ConstIterator(m_table.end(), m_table.end()); }

private:
    void prune_dead_entries()
    {
        m_table.remove_all_matching([](Weak<T> const& entry) {
            return !entry.ptr();
        });
    }

    TableType m_table;
};

}
