/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/SetIterator.h>

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
}

void SetIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_set);
}

BuiltinIterator* SetIterator::as_builtin_iterator_if_next_is_not_redefined(IteratorRecord const& iterator_record)
{
    if (iterator_record.next_method.is_object()) {
        auto const& next_function = iterator_record.next_method.as_object();
        if (next_function.is_native_function()) {
            auto const& native_function = static_cast<NativeFunction const&>(next_function);
            if (native_function.is_set_prototype_next_builtin())
                return this;
        }
    }
    return nullptr;
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
