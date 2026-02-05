/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseReaction.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

GC_DEFINE_ALLOCATOR(PromiseReaction);

GC::Ref<PromiseReaction> PromiseReaction::create(VM& vm, Type type, GC::Ptr<PromiseCapability> capability, GC::Ptr<JobCallback> handler)
{
    return vm.heap().allocate<PromiseReaction>(type, capability, move(handler));
}

PromiseReaction::PromiseReaction(Type type, GC::Ptr<PromiseCapability> capability, GC::Ptr<JobCallback> handler)
    : m_type(type)
    , m_capability(capability)
    , m_handler(move(handler))
{
}

void PromiseReaction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_capability);
    visitor.visit(m_handler);
}

}
