/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/InjectionSink.h>

#include <AK/Utf16String.h>

namespace Web::TrustedTypes {

Utf16String to_string(InjectionSink sink)
{
    switch (sink) {
#define __ENUMERATE_INJECTION_SINKS(name, value) \
    case InjectionSink::name:                    \
        return value##_utf16;
        ENUMERATE_INJECTION_SINKS
#undef __ENUMERATE_INJECTION_SINKS
    default:
        VERIFY_NOT_REACHED();
    }
}

}
