/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibJS/Runtime/SetIteratorPrototype.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SetIterator);

GC::Ref<SetIterator> SetIterator::create(Realm& realm, Set& set, Object::PropertyKind iteration_kind)
{
    return realm.create<SetIterator>(set, iteration_kind, realm.intrinsics().set_iterator_prototype());
}

SetIterator::SetIterator(Set& set, Object::PropertyKind iteration_kind, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_set(set)
    , m_iteration_kind(iteration_kind)
    , m_iterator(static_cast<Set const&>(set).begin())
{
    auto& set_iterator_prototype = as<SetIteratorPrototype>(prototype);
    m_next_method_was_redefined = set_iterator_prototype.next_method_was_redefined();
}

void SetIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set);
}

ThrowCompletionOr<void> SetIterator::next(VM& vm, bool& done, Value& value)
{
    if (m_done) {
        done = true;
        value = js_undefined();
        return {};
    }

    if (m_iterator == m_set->end()) {
        m_done = true;
        done = true;
        value = js_undefined();
        return {};
    }

    VERIFY(m_iteration_kind != Object::PropertyKind::Key);

    value = (*m_iterator).key;
    ++m_iterator;
    if (m_iteration_kind == Object::PropertyKind::Value) {
        return {};
    }

    value = Array::create_from(*vm.current_realm(), { value, value });
    return {};
}

}
