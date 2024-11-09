/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/RegExpStringIterator.h>

namespace JS {

class RegExpStringIteratorPrototype final : public PrototypeObject<RegExpStringIteratorPrototype, RegExpStringIterator> {
    JS_PROTOTYPE_OBJECT(RegExpStringIteratorPrototype, RegExpStringIterator, RegExpStringIterator);
    JS_DECLARE_ALLOCATOR(RegExpStringIteratorPrototype);

public:
    virtual ~RegExpStringIteratorPrototype() override = default;

    virtual void initialize(Realm&) override;

private:
    explicit RegExpStringIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
};

}
