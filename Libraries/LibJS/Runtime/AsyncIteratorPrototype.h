/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class AsyncIteratorPrototype final : public Object {
    JS_OBJECT(AsyncIteratorPrototype, Object)
    JS_DECLARE_ALLOCATOR(AsyncIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~AsyncIteratorPrototype() override = default;

private:
    explicit AsyncIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(symbol_async_iterator);
};

}
