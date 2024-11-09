/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS::Intl {

class Intl final : public Object {
    JS_OBJECT(Intl, Object);
    JS_DECLARE_ALLOCATOR(Intl);

public:
    virtual void initialize(Realm&) override;
    virtual ~Intl() override = default;

private:
    explicit Intl(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(get_canonical_locales);
    JS_DECLARE_NATIVE_FUNCTION(supported_values_of);
};

}
