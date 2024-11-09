/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

struct AlreadyResolved final : public Cell {
    JS_CELL(AlreadyResolved, Cell);
    JS_DECLARE_ALLOCATOR(AlreadyResolved);

    bool value { false };

protected:
    // Allocated cells must be >= sizeof(FreelistEntry), which is 24 bytes -
    // but AlreadyResolved is only 16 bytes without this.
    u8 dummy[8];
};

class PromiseResolvingFunction final : public NativeFunction {
    JS_OBJECT(PromiseResolvingFunction, NativeFunction);
    JS_DECLARE_ALLOCATOR(PromiseResolvingFunction);

public:
    using FunctionType = Function<Value(VM&, Promise&, AlreadyResolved&)>;

    static NonnullGCPtr<PromiseResolvingFunction> create(Realm&, Promise&, AlreadyResolved&, FunctionType);

    virtual void initialize(Realm&) override;
    virtual ~PromiseResolvingFunction() override = default;

    virtual ThrowCompletionOr<Value> call() override;

private:
    explicit PromiseResolvingFunction(Promise&, AlreadyResolved&, FunctionType, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    NonnullGCPtr<Promise> m_promise;
    NonnullGCPtr<AlreadyResolved> m_already_resolved;
    FunctionType m_native_function;
};

}
