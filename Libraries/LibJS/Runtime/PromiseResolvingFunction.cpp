/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseResolvingFunction.h>

namespace JS {

GC_DEFINE_ALLOCATOR(PromiseResolvingFunction);

GC::Ref<PromiseResolvingFunction> PromiseResolvingFunction::create_resolve(Realm& realm, Promise& promise)
{
    return realm.create<PromiseResolvingFunction>(promise, Kind::Resolve, nullptr, realm.intrinsics().function_prototype());
}

GC::Ref<PromiseResolvingFunction> PromiseResolvingFunction::create_reject(Realm& realm, Promise& promise, PromiseResolvingFunction& resolve_function)
{
    return realm.create<PromiseResolvingFunction>(promise, Kind::Reject, &resolve_function, realm.intrinsics().function_prototype());
}

PromiseResolvingFunction::PromiseResolvingFunction(Promise& promise, Kind kind, GC::Ptr<PromiseResolvingFunction> resolve_function, Object& prototype)
    : NativeFunction(prototype)
    , m_promise(promise)
    , m_resolve_function(resolve_function)
    , m_kind(kind)
{
}

void PromiseResolvingFunction::initialize(Realm& realm)
{
    Base::initialize(realm);
    define_direct_property(vm().names.length, Value(1), Attribute::Configurable);
}

ThrowCompletionOr<Value> PromiseResolvingFunction::call()
{
    switch (m_kind) {
    case Kind::Resolve:
        return Promise::resolve_function_steps(vm(), m_promise, already_resolved());
    case Kind::Reject:
        return Promise::reject_function_steps(vm(), m_promise, already_resolved());
    }
    VERIFY_NOT_REACHED();
}

void PromiseResolvingFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_promise);
    if (m_resolve_function)
        visitor.visit(m_resolve_function);
}

bool& PromiseResolvingFunction::already_resolved()
{
    if (m_kind == Kind::Resolve)
        return m_already_resolved;

    VERIFY(m_resolve_function);
    return m_resolve_function->m_already_resolved;
}

}
