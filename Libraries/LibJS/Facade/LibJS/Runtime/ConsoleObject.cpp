/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Console.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ConsoleObject.h>

namespace JS {

using namespace EmbeddingABI;

Console& ConsoleObject::console()
{
    auto* console = js_console_object_console(object_to_abi(*this));
    VERIFY(console);
    return *cell_from_abi<Console>(console);
}

}
