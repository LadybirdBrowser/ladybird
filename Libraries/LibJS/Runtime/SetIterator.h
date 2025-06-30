/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Set.h>

namespace JS {

class JS_API SetIterator final : public Object
    , public BuiltinIterator {
    JS_OBJECT(SetIterator, Object);
    GC_DECLARE_ALLOCATOR(SetIterator);

public:
    static GC::Ref<SetIterator> create(Realm&, Set& set, Object::PropertyKind iteration_kind);

    virtual ~SetIterator() override = default;

    BuiltinIterator* as_builtin_iterator_if_next_is_not_redefined(IteratorRecord const&) override;
    ThrowCompletionOr<void> next(VM&, bool& done, Value& value) override;

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
