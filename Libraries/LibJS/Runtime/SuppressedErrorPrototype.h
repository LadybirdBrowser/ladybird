/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class SuppressedErrorPrototype final : public Object {
    JS_OBJECT(SuppressedErrorPrototype, Object);
    JS_DECLARE_ALLOCATOR(SuppressedErrorPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~SuppressedErrorPrototype() override = default;

private:
    explicit SuppressedErrorPrototype(Realm&);
};

}
