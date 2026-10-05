/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Array.h>

namespace JS {

// %Array.prototype%, which LibJS's users reach through Intrinsics::array_prototype().
class ArrayPrototype final : public Array {
};

}
