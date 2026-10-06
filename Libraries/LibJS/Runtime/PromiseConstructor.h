/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

class PromiseConstructor final : public NativeFunction {
public:
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_PROMISE_CONSTRUCTOR; }
};

}
