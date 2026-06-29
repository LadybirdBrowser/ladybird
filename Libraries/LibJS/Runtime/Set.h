/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class JS_API Set : public Object {
    JS_OBJECT(Set, Object);
    GC_DECLARE_ALLOCATOR(Set);

public:
    static GC::Ref<Set> create(Realm&);

    virtual void initialize(Realm&) override;
    virtual ~Set() override = default;

    virtual bool is_set_object() const final { return true; }

    // NOTE: Unlike what the spec says, we implement Sets using an underlying map,
    //       so all the functions below do not directly implement the operations as
    //       defined by the specification.

    void set_clear() { m_values->map_clear(); }
    bool set_remove(Value const& value) { return m_values->map_remove(value); }
    bool set_has(Value const& key) const { return m_values->map_has(key); }
    void set_add(Value const& key) { m_values->map_set(key, js_undefined()); }
    size_t set_size() const { return m_values->map_size(); }

    struct EndIterator {
    };

    class ConstIterator {
    public:
        ConstIterator& operator++()
        {
            ++m_iterator;
            return *this;
        }

        Value operator*() const { return (*m_iterator).key; }

        bool operator==(ConstIterator const& other) const { return m_iterator == other.m_iterator; }
        bool operator==(EndIterator const&) const { return m_iterator == Map::EndIterator {}; }

        void visit_edges(Cell::Visitor& visitor) { m_iterator.visit_edges(visitor); }

    private:
        friend class Set;

        explicit ConstIterator(Map::ConstIterator iterator)
            : m_iterator(iterator)
        {
        }

        Map::ConstIterator m_iterator;
    };

    ConstIterator begin() const { return ConstIterator { const_cast<Map const&>(*m_values).begin() }; }
    ConstIterator begin() { return ConstIterator { const_cast<Map const&>(*m_values).begin() }; }
    EndIterator end() const { return {}; }

    GC::Ref<Set> copy() const;

private:
    explicit Set(Object& prototype);

    virtual void visit_edges(Visitor& visitor) override;

    GC::Ptr<Map> m_values;
};

// 24.2.1.1 Set Records, https://tc39.es/ecma262/#sec-set-records
struct SetRecord {
    GC::Ref<Object const> set_object; // [[SetObject]]
    double size { 0 };                // [[Size]
    GC::Ref<FunctionObject> has;      // [[Has]]
    GC::Ref<FunctionObject> keys;     // [[Keys]]
};

ThrowCompletionOr<SetRecord> get_set_record(VM&, Value);
bool set_data_has(GC::Ref<Set>, Value);

template<>
inline bool Object::fast_is<Set>() const { return is_set_object(); }

}
