/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS::Intl {

class CollatorCompareFunction : public NativeFunction {
    JS_OBJECT(CollatorCompareFunction, NativeFunction);
    JS_DECLARE_ALLOCATOR(CollatorCompareFunction);

public:
    static NonnullGCPtr<CollatorCompareFunction> create(Realm&, Collator&);

    virtual void initialize(Realm&) override;
    virtual ~CollatorCompareFunction() override = default;

    virtual ThrowCompletionOr<Value> call() override;

private:
    CollatorCompareFunction(Realm&, Collator&);

    virtual void visit_edges(Visitor&) override;

    NonnullGCPtr<Collator> m_collator; // [[Collator]]
};

int compare_strings(Collator const&, StringView x, StringView y);

}
