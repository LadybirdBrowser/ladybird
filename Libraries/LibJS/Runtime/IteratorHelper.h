/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class IteratorHelper final : public GeneratorObject {
    JS_OBJECT(IteratorHelper, GeneratorObject);
    GC_DECLARE_ALLOCATOR(IteratorHelper);

public:
    using Closure = GC::Function<ThrowCompletionOr<IterationResult>(VM&, IteratorHelper&)>;
    using AbruptClosure = GC::Function<ThrowCompletionOr<Value>(VM&, Completion const&)>;

    static GC::Ref<IteratorHelper> create(Realm&, ReadonlySpan<GC::Ref<IteratorRecord>>, GC::Ref<Closure>, GC::Ptr<AbruptClosure> = {});

    ReadonlySpan<GC::Ref<IteratorRecord>> underlying_iterators() const { return m_underlying_iterators; }

    size_t counter() const { return m_counter; }
    void increment_counter() { ++m_counter; }

private:
    IteratorHelper(Realm&, Object& prototype, ReadonlySpan<GC::Ref<IteratorRecord>>, GC::Ref<Closure>, GC::Ptr<AbruptClosure>);

    virtual void visit_edges(Visitor&) override;
    virtual ThrowCompletionOr<IterationResult> execute(VM&, JS::Completion const& completion) override;

    Vector<GC::Ref<IteratorRecord>> m_underlying_iterators; // [[UnderlyingIterators]]
    GC::Ref<Closure> m_closure;
    GC::Ptr<AbruptClosure> m_abrupt_closure;

    size_t m_counter { 0 };
    bool m_done { false };
};

}
