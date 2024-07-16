/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Value.h>

namespace JS {

class Set : public Object {
    JS_OBJECT(Set, Object);
    JS_DECLARE_ALLOCATOR(Set);

public:
    static NonnullGCPtr<Set> create(Realm&);

    virtual void initialize(Realm&) override;
    virtual ~Set() override = default;

    // NOTE: Unlike what the spec says, we implement Sets using an underlying map,
    //       so all the functions below do not directly implement the operations as
    //       defined by the specification.

    void set_clear() { m_values->map_clear(); }
    bool set_remove(Value const& value) { return m_values->map_remove(value); }
    bool set_has(Value const& key) const { return m_values->map_has(key); }
    void set_add(Value const& key) { m_values->map_set(key, js_undefined()); }
    size_t set_size() const { return m_values->map_size(); }

    auto begin() const { return const_cast<Map const&>(*m_values).begin(); }
    auto begin() { return m_values->begin(); }
    auto end() const { return m_values->end(); }

    NonnullGCPtr<Set> copy() const;

private:
    explicit Set(Object& prototype);

    virtual void visit_edges(Visitor& visitor) override;

    GCPtr<Map> m_values;
};

// 24.2.1.1 Set Records, https://tc39.es/ecma262/#sec-set-records
struct SetRecord {
    NonnullGCPtr<Object const> set_object; // [[SetObject]]
    double size { 0 };                     // [[Size]
    NonnullGCPtr<FunctionObject> has;      // [[Has]]
    NonnullGCPtr<FunctionObject> keys;     // [[Keys]]
};

ThrowCompletionOr<SetRecord> get_set_record(VM&, Value);
bool set_data_has(NonnullGCPtr<Set>, Value);

}
