/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Value.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#extract-error
struct ErrorInformation {
    String message;
    String filename;
    JS::Value error;
    size_t lineno { 0 };
    size_t colno { 0 };
};

ErrorInformation extract_error_information(JS::VM&, JS::Value exception);

}
