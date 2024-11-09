/*
 * Copyright (c) 2024, Gasim Gasimzada <gasim@gasimzada.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class ConsoleObjectPrototype final : public Object {
    JS_OBJECT(ConsoleObjectPrototype, Object);
    JS_DECLARE_ALLOCATOR(ConsoleObjectPrototype);

public:
    virtual void initialize(JS::Realm&) override;
    virtual ~ConsoleObjectPrototype() override = default;

private:
    explicit ConsoleObjectPrototype(JS::Realm&);
};

}
