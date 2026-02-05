/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Script.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    auto js = StringView(static_cast<unsigned char const*>(data), size);
    // FIXME: https://github.com/SerenityOS/serenity/issues/17899
    if (!Utf8View(js).validate())
        return 0;
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;
    auto parse_result = JS::Script::parse(js, realm);
    if (!parse_result.is_error())
        (void)vm->bytecode_interpreter().run(parse_result.value());

    return 0;
}
