/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/InterfaceObject.h>

namespace Web::Bindings {

struct ImageConstructor {
    static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct(JS::HostFunction&, JS::FunctionObject& new_target);
};

}
