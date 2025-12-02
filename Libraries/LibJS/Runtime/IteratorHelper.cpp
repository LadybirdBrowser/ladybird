/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/IteratorHelper.h>
#include <LibJS/Runtime/Realm.h>

namespace JS {

GC_DEFINE_ALLOCATOR(IteratorHelper);

GC::Ref<IteratorHelper> IteratorHelper::create(Realm& realm, ReadonlySpan<GC::Ref<IteratorRecord>> underlying_iterators, GC::Ref<Closure> closure, GC::Ptr<AbruptClosure> abrupt_closure)
{
    return realm.create<IteratorHelper>(realm, realm.intrinsics().iterator_helper_prototype(), underlying_iterators, closure, abrupt_closure);
}

IteratorHelper::IteratorHelper(Realm& realm, Object& prototype, ReadonlySpan<GC::Ref<IteratorRecord>> underlying_iterators, GC::Ref<Closure> closure, GC::Ptr<AbruptClosure> abrupt_closure)
    : GeneratorObject(realm, &prototype, realm.vm().running_execution_context().copy(), "Iterator Helper"sv)
    , m_underlying_iterators(underlying_iterators)
    , m_closure(closure)
    , m_abrupt_closure(abrupt_closure)
{
}

void IteratorHelper::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_underlying_iterators);
    visitor.visit(m_closure);
    visitor.visit(m_abrupt_closure);
}

ThrowCompletionOr<IteratorHelper::IterationResult> IteratorHelper::execute(VM& vm, JS::Completion const& completion)
{
    ScopeGuard guard { [&] { vm.pop_execution_context(); } };

    if (completion.is_abrupt()) {
        auto abrupt_result = m_abrupt_closure
            ? TRY(m_abrupt_closure->function()(vm, completion))
            : TRY(iterator_close_all(vm, underlying_iterators(), completion));

        set_generator_state(GeneratorState::Completed);
        return IterationResult(abrupt_result, true);
    }

    auto result_value = m_closure->function()(vm, *this);

    if (result_value.is_throw_completion()) {
        set_generator_state(GeneratorState::Completed);
        return result_value.throw_completion();
    }

    auto result = result_value.release_value();
    set_generator_state(result.done ? GeneratorState::Completed : GeneratorState::SuspendedYield);

    return result;
}

}
