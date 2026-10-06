/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

// %AsyncIteratorPrototype%, which LibJS's users reach through Intrinsics::async_iterator_prototype().
class AsyncIteratorPrototype final : public Object {
};

}
