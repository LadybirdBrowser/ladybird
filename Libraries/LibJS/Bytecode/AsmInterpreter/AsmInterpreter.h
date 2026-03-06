/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS::Bytecode {

class Interpreter;

class AsmInterpreter {
public:
    static void run(Interpreter&, size_t entry_point);
    static bool is_available();
};

}
