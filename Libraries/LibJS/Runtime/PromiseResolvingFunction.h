/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class PromiseResolvingFunction final : public NativeFunction {
    JS_OBJECT(PromiseResolvingFunction, NativeFunction);
    GC_DECLARE_ALLOCATOR(PromiseResolvingFunction);

public:
    enum class Kind : u8 {
        Resolve,
        Reject,
    };

    static GC::Ref<PromiseResolvingFunction> create_resolve(Realm&, Promise&);
    static GC::Ref<PromiseResolvingFunction> create_reject(Realm&, Promise&, PromiseResolvingFunction& resolve_function);

    virtual void initialize(Realm&) override;
    virtual ~PromiseResolvingFunction() override = default;

    virtual ThrowCompletionOr<Value> call() override;

private:
    explicit PromiseResolvingFunction(Promise&, Kind, GC::Ptr<PromiseResolvingFunction>, Object& prototype);

    virtual void visit_edges(Visitor&) override;
    bool& already_resolved();

    GC::Ref<Promise> m_promise;
    GC::Ptr<PromiseResolvingFunction> m_resolve_function;
    bool m_already_resolved { false };
    Kind m_kind;
};

}
