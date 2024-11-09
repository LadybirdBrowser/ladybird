/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/IteratorHelper.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS {

class IteratorHelperPrototype final : public PrototypeObject<IteratorHelperPrototype, IteratorHelper> {
    JS_PROTOTYPE_OBJECT(IteratorHelperPrototype, IteratorHelper, IteratorHelper);
    JS_DECLARE_ALLOCATOR(IteratorHelperPrototype);

public:
    virtual void initialize(Realm&) override;

private:
    explicit IteratorHelperPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
    JS_DECLARE_NATIVE_FUNCTION(return_);
};

}
