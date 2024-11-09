/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/IteratorHelper.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

JS_DEFINE_ALLOCATOR(IteratorHelper);

ThrowCompletionOr<NonnullGCPtr<IteratorHelper>> IteratorHelper::create(Realm& realm, NonnullGCPtr<IteratorRecord> underlying_iterator, NonnullGCPtr<Closure> closure, GCPtr<AbruptClosure> abrupt_closure)
{
    return realm.heap().allocate<IteratorHelper>(realm, realm, realm.intrinsics().iterator_helper_prototype(), move(underlying_iterator), closure, abrupt_closure);
}

IteratorHelper::IteratorHelper(Realm& realm, Object& prototype, NonnullGCPtr<IteratorRecord> underlying_iterator, NonnullGCPtr<Closure> closure, GCPtr<AbruptClosure> abrupt_closure)
    : GeneratorObject(realm, prototype, realm.vm().running_execution_context().copy(), "Iterator Helper"sv)
    , m_underlying_iterator(move(underlying_iterator))
    , m_closure(closure)
    , m_abrupt_closure(abrupt_closure)
{
}

void IteratorHelper::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_underlying_iterator);
    visitor.visit(m_closure);
    visitor.visit(m_abrupt_closure);
}

Value IteratorHelper::result(Value value)
{
    set_generator_state(value.is_undefined() ? GeneratorState::Completed : GeneratorState::SuspendedYield);
    return value;
}

ThrowCompletionOr<Value> IteratorHelper::close_result(VM& vm, Completion completion)
{
    set_generator_state(GeneratorState::Completed);
    return *TRY(iterator_close(vm, underlying_iterator(), move(completion)));
}

ThrowCompletionOr<Value> IteratorHelper::execute(VM& vm, JS::Completion const& completion)
{
    ScopeGuard guard { [&] { vm.pop_execution_context(); } };

    if (completion.is_abrupt()) {
        if (m_abrupt_closure)
            return m_abrupt_closure->function()(vm, *this, completion);
        return close_result(vm, completion);
    }

    auto result_value = m_closure->function()(vm, *this);

    if (result_value.is_throw_completion()) {
        set_generator_state(GeneratorState::Completed);
        return result_value;
    }

    return create_iterator_result_object(vm, result(result_value.release_value()), generator_state() == GeneratorState::Completed);
}

}
