/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/SetIterator.h>
#include <LibJS/Runtime/SetPrototype.h>

namespace JS {

bool set_iteration_is_unobservable(Realm& realm, FunctionObject const& iterator_method)
{
    auto& set_prototype = static_cast<SetPrototype const&>(*realm.intrinsics().set_prototype());
    if (&iterator_method != &set_prototype.values_function())
        return false;

    // NB: Inspect the intrinsic prototype's own property without invoking it. An accessor or a replacement method
    //     is observable, so it makes the caller take the generic path.
    static auto& next_method_cache = *new Bytecode::StaticPropertyLookupCache;
    auto next_method = Bytecode::get_own_property_without_side_effects(*realm.intrinsics().set_iterator_prototype(), realm.vm().names.next, next_method_cache);
    auto native_function = next_method.as_if<NativeFunction>();
    return native_function && native_function->is_set_prototype_next_builtin();
}

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
    m_iterator.visit_edges(visitor);
}

BuiltinIterator* SetIterator::as_builtin_iterator_if_next_is_not_redefined(Value next_method)
{
    if (auto native_function = next_method.as_if<NativeFunction>()) {
        if (native_function->is_set_prototype_next_builtin())
            return this;
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

    value = *m_iterator;
    ++m_iterator;
    if (m_iteration_kind == Object::PropertyKind::Value) {
        return {};
    }

    value = Array::create_from(*vm.current_realm(), { value, value });
    return {};
}

}
