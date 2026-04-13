/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS {

class VM;
namespace Bytecode {

class AsmInterpreter {
public:
    static void run(VM&, size_t entry_point);
    static bool is_available();
};

}
}
