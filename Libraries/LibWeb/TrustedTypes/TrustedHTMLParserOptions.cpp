/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>

namespace Web::TrustedTypes {

extern "C" {
// Defined in Bindings/Rust/src/trusted_types/trusted_html_parser_options.rs.
bool web_rust_trusted_types_trusted_html_parser_options_run_scripts(TrustedHTMLParserOptions*);
}

bool TrustedHTMLParserOptions::run_scripts() const
{
    return web_rust_trusted_types_trusted_html_parser_options_run_scripts(const_cast<TrustedHTMLParserOptions*>(this));
}

}
