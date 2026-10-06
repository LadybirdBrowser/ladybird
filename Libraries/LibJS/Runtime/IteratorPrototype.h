/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

// %Iterator.prototype%, which LibJS's users reach through Intrinsics::iterator_prototype().
class IteratorPrototype : public Object {
};

}
