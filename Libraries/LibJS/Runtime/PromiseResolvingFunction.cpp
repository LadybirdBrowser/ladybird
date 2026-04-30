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

GC_DEFINE_ALLOCATOR(AlreadyResolved);
GC_DEFINE_ALLOCATOR(PromiseResolvingFunction);

GC::Ref<PromiseResolvingFunction> PromiseResolvingFunction::create(Realm& realm, Promise& promise, AlreadyResolved& already_resolved, Kind kind)
{
    return realm.create<PromiseResolvingFunction>(promise, already_resolved, kind, realm.intrinsics().function_prototype());
}

PromiseResolvingFunction::PromiseResolvingFunction(Promise& promise, AlreadyResolved& already_resolved, Kind kind, Object& prototype)
    : NativeFunction(prototype)
    , m_promise(promise)
    , m_already_resolved(already_resolved)
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
        return Promise::resolve_function_steps(vm(), m_promise, m_already_resolved);
    case Kind::Reject:
        return Promise::reject_function_steps(vm(), m_promise, m_already_resolved);
    }
    VERIFY_NOT_REACHED();
}

void PromiseResolvingFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_promise);
    visitor.visit(m_already_resolved);
}

}
