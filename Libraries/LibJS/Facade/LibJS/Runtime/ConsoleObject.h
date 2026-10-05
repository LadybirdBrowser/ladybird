/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Embedding/Layout.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API ConsoleObject final : public Object {
public:
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_CONSOLE_OBJECT; }

    Console& console();
};

}
