/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/InterfaceObject.h>

namespace Web::Bindings {

struct AudioConstructor {
    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(InterfaceConstructor&, JS::FunctionObject& new_target);
};

}
