/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Set.h>

namespace JS {

class SetIterator final : public Object {
    JS_OBJECT(SetIterator, Object);
    GC_DECLARE_ALLOCATOR(SetIterator);

public:
    static GC::Ref<SetIterator> create(Realm&, Set& set, Object::PropertyKind iteration_kind);

    virtual ~SetIterator() override = default;

    Set& set() const { return m_set; }
    bool done() const { return m_done; }
    Object::PropertyKind iteration_kind() const { return m_iteration_kind; }

private:
    friend class SetIteratorPrototype;

    explicit SetIterator(Set& set, Object::PropertyKind iteration_kind, Object& prototype);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Set> m_set;
    bool m_done { false };
    Object::PropertyKind m_iteration_kind;
    Map::ConstIterator m_iterator;
};

}
