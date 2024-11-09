/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class NumberFormatFunction final : public NativeFunction {
    JS_OBJECT(NumberFormatFunction, NativeFunction);
    JS_DECLARE_ALLOCATOR(NumberFormatFunction);

public:
    static NonnullGCPtr<NumberFormatFunction> create(Realm&, NumberFormat&);

    virtual ~NumberFormatFunction() override = default;
    virtual void initialize(Realm&) override;

    virtual ThrowCompletionOr<Value> call() override;

private:
    explicit NumberFormatFunction(NumberFormat&, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    NonnullGCPtr<NumberFormat> m_number_format; // [[NumberFormat]]
};

}
