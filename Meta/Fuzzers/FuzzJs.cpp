/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
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
    auto source_text = Utf16String::from_utf8_without_validation(js);
    auto vm = JS::VM::create();
    auto root_execution_context = JS::create_simple_execution_context<JS::GlobalObject>(*vm);
    auto& realm = *root_execution_context->realm;
    auto parse_result = JS::Script::parse(source_text.utf16_view(), realm);
    if (!parse_result.is_error())
        (void)vm->run(parse_result.value());

    return 0;
}
