/*
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class BigIntPrototype final : public Object {
    JS_OBJECT(BigIntPrototype, Object);
    JS_DECLARE_ALLOCATOR(BigIntPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~BigIntPrototype() override = default;

private:
    explicit BigIntPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
};

}
