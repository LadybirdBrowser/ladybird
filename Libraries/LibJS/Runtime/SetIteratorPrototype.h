/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/SetIterator.h>

namespace JS {

class SetIteratorPrototype final : public PrototypeObject<SetIteratorPrototype, SetIterator> {
    JS_PROTOTYPE_OBJECT(SetIteratorPrototype, SetIterator, SetIterator);
    JS_DECLARE_ALLOCATOR(SetIteratorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SetIteratorPrototype() override = default;

private:
    explicit SetIteratorPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(next);
};

}
