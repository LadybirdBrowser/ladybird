/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Heap/EngineCell.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Symbol.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

// The getter and setter of an accessor property, which the runtime keeps in a cell of its own. LibJS's users only name
// the type.
class Accessor final : public EngineCell {
};

}
