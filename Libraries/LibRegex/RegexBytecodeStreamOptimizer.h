/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Forward.h"
#include <AK/Vector.h>

namespace regex {

class Optimizer {
public:
    static void append_alternation(ByteCode& target, ByteCode&& left, ByteCode&& right);
    static void append_alternation(ByteCode& target, Span<ByteCode> alternatives);
    static void append_character_class(ByteCode& target, Vector<CompareTypeAndValuePair>&& pairs);
};

}
