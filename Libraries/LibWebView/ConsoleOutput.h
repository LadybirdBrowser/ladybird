/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibJS/Console.h>

namespace WebView {

struct ConsoleOutput {
    JS::Console::LogLevel level;
    UnixDateTime timestamp;
    Vector<JsonValue> arguments;
};

}
