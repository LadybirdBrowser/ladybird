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
        auto string = argument.to_string_without_side_effects();
        for (auto code_point : string.code_points()) {
            if (code_point < 0x20) {
                builder.appendff("\\u{:04x}", code_point);
                continue;
            }
            builder.append_code_point(code_point);
        }
        if (argument.is_string())
            builder.append_code_point('"');
    }
    dbgln("{}({})", function, builder.string_view());
}

}
