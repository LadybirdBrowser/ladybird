/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class AggregateError : public Error {
    JS_OBJECT(AggregateError, Error);
    JS_DECLARE_ALLOCATOR(AggregateError);

public:
    static NonnullGCPtr<AggregateError> create(Realm&);
    virtual ~AggregateError() override = default;

private:
    explicit AggregateError(Object& prototype);
};

}
