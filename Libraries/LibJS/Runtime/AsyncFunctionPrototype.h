/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class AsyncFunctionPrototype final : public Object {
    JS_OBJECT(AsyncFunctionPrototype, Object);
    JS_DECLARE_ALLOCATOR(AsyncFunctionPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~AsyncFunctionPrototype() override = default;

private:
    explicit AsyncFunctionPrototype(Realm&);
};

}
