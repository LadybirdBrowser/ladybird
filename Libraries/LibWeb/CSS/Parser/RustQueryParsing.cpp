/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/ValueParserRustFFI.h>

namespace Web::CSS::Parser {

using namespace ValueParserFFI;

Optional<RustQueryHandle> Parser::parse_as_supports(Utf16View source)
{
    auto context = make_parse_context(ParseContextMode::Syntax);
    auto* handle = rust_parse_supports_condition(ffi_utf16_view(source), &context.context);
    if (!handle)
        return {};
    return RustQueryHandle { handle };
}

}
