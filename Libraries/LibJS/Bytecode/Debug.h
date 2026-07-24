/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>

namespace JS::Bytecode {

JS_API extern bool g_dump_bytecode;
JS_API bool should_dump_interpreter_assembly();
JS_API bool should_dump_bytecode();

}
