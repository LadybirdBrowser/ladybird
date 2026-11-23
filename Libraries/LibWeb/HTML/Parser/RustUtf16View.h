/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/HTMLTokenizerRustFFI.h>

namespace Web::HTML {

// A view of code units the Rust HTML parser hands over, Latin-1 or UTF-16 as it stores them. They are only there
// until the call they were passed to returns.
inline Utf16View utf16_view_from_ffi(RustFfiHtmlUtf16View view)
{
    if (view.length == 0)
        return {};
    if (view.latin1)
        return Utf16View::from_latin1({ view.latin1, view.length }, false);
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

}
