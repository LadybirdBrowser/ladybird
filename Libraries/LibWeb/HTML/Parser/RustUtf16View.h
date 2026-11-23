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

// A view of code units for the Rust HTML parser to read until the call they are passed to returns.
inline RustFfiHtmlUtf16View utf16_view_to_ffi(Utf16View view)
{
    if (view.has_latin1_storage())
        return { view.latin1_span().data(), nullptr, view.length_in_code_units() };
    return { nullptr, reinterpret_cast<u16 const*>(view.utf16_span().data()), view.length_in_code_units() };
}

}
