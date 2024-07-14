/*
 * Copyright (c) 2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>

namespace Web::WebIDL {

extern bool g_enable_idl_tracing;

void log_trace_impl(JS::VM&, char const*);

inline void log_trace(JS::VM& vm, char const* function)
{
    if (g_enable_idl_tracing)
        log_trace_impl(vm, function);
}

}
