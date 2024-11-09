/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RedBlackTree.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueTraits.h>

namespace JS {

class Map : public Object {
    JS_OBJECT(Map, Object);
    JS_DECLARE_ALLOCATOR(Map);

public:
    static NonnullGCPtr<Map> create(Realm&);

    virtual ~Map() override = default;

    void map_clear();
    bool map_remove(Value const&);
    Optional<Value> map_get(Value const&) const;
    bool map_has(Value const&) const;
    void map_set(Value const&, Value);
    size_t map_size() const;

    struct EndIterator {
    };

    template<bool IsConst>
    struct IteratorImpl {
        bool is_end() const
        {
            return m_map->m_keys.begin_from(m_index).is_end()
                && m_map->m_keys.find_smallest_not_below_iterator(m_index).is_end();
        }

        IteratorImpl& operator++()
        {
            ++m_index;
            return *this;
        }

        decltype(auto) operator*()
        {
            ensure_next_element();
            return *m_map->m_entries.find(*m_map->m_keys.begin_from(m_index));
        }

        decltype(auto) operator*() const
        {
            ensure_next_element();
            return *m_map->m_entries.find(*m_map->m_keys.begin_from(m_index));
        }

        bool operator==(IteratorImpl const& other) const { return m_index == other.m_index && &m_map == &other.m_map; }
        bool operator==(EndIterator const&) const { return is_end(); }

    private:
        friend class Map;
        IteratorImpl(Map const& map)
        requires(IsConst)
            : m_map(map)
        {
            ensure_index();
        }

        IteratorImpl(Map& map)
        requires(!IsConst)
            : m_map(map)
        {
            ensure_index();
        }

        void ensure_index() const
        {
            if (m_map->m_keys.is_empty())
                m_index = m_map->m_next_insertion_id;
            else
                m_index = m_map->m_keys.begin().key();
        }

        void ensure_next_element() const
        {
            if (auto it = m_map->m_keys.find_smallest_not_below_iterator(m_index); it.is_end())
                m_index = m_map->m_next_insertion_id;
            else
                m_index = it.key();
        }

        Conditional<IsConst, NonnullGCPtr<Map const>, NonnullGCPtr<Map>> m_map;
        mutable size_t m_index { 0 };
    };

    using Iterator = IteratorImpl<false>;
    using ConstIterator = IteratorImpl<true>;

    ConstIterator begin() const { return { *this }; }
    Iterator begin() { return { *this }; }
    EndIterator end() const { return {}; }

private:
    explicit Map(Object& prototype);
    virtual void visit_edges(Visitor& visitor) override;

    size_t m_next_insertion_id { 0 };
    RedBlackTree<size_t, Value> m_keys;
    HashMap<Value, Value, ValueTraits> m_entries;
};

}
