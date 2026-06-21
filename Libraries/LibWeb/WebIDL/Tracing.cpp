/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebIDL/Tracing.h>

namespace Web::WebIDL {

bool g_enable_idl_tracing = false;

static void log_trace_impl(JS::VM&, char const*);
void log_trace_impl(JS::VM& vm, char const* function)
{
    if (!g_enable_idl_tracing)
        return;

    StringBuilder builder;
    for (size_t i = 0; i < vm.argument_count(); ++i) {
        if (i != 0)
            builder.append(", "sv);
        auto argument = vm.argument(i);
        if (argument.is_string())
            builder.append_code_point('"');
        auto string = argument.to_utf16_string_without_side_effects();
        auto view = string.utf16_view();
        for (size_t code_unit_index = 0; code_unit_index < view.length_in_code_units(); ++code_unit_index) {
            auto code_unit = view.code_unit_at(code_unit_index);
            if (code_unit < 0x20) {
                builder.appendff("\\u{:04x}", code_unit);
                continue;
            }
            builder.append_code_unit(code_unit);
        }
        if (argument.is_string())
            builder.append_code_point('"');
    }
    dbgln("{}({})", function, builder.string_view());
}

void log_trace(JS::VM& vm, char const* function)
{
    if (g_enable_idl_tracing)
        log_trace_impl(vm, function);
}

void set_enable_idl_tracing(bool const enabled)
{
    g_enable_idl_tracing = enabled;
}

}
