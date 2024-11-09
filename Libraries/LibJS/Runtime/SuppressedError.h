/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Error.h>

namespace JS {

class SuppressedError : public Error {
    JS_OBJECT(SuppressedError, Error);
    JS_DECLARE_ALLOCATOR(SuppressedError);

public:
    static NonnullGCPtr<SuppressedError> create(Realm&);
    virtual ~SuppressedError() override = default;

private:
    explicit SuppressedError(Object& prototype);
};

}
