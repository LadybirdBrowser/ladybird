/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
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
    using Closure = GC::Function<ThrowCompletionOr<Value>(VM&, IteratorHelper&)>;
    using AbruptClosure = GC::Function<ThrowCompletionOr<Value>(VM&, IteratorHelper&, Completion const&)>;

    static ThrowCompletionOr<GC::Ref<IteratorHelper>> create(Realm&, GC::Ref<IteratorRecord>, GC::Ref<Closure>, GC::Ptr<AbruptClosure> = {});

    IteratorRecord& underlying_iterator() { return m_underlying_iterator; }
    IteratorRecord const& underlying_iterator() const { return m_underlying_iterator; }

    size_t counter() const { return m_counter; }
    void increment_counter() { ++m_counter; }

    Value result(Value);
    ThrowCompletionOr<Value> close_result(VM&, Completion);

private:
    IteratorHelper(Realm&, Object& prototype, GC::Ref<IteratorRecord>, GC::Ref<Closure>, GC::Ptr<AbruptClosure>);

    virtual void visit_edges(Visitor&) override;
    virtual ThrowCompletionOr<Value> execute(VM&, JS::Completion const& completion) override;

    GC::Ref<IteratorRecord> m_underlying_iterator; // [[UnderlyingIterator]]
    GC::Ref<Closure> m_closure;
    GC::Ptr<AbruptClosure> m_abrupt_closure;

    size_t m_counter { 0 };
    bool m_done { false };
};

}
