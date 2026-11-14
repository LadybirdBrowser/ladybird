/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TrustedHTMLParserOptions.h>

namespace Web::TrustedTypes {

class TrustedHTMLParserOptions final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TrustedHTMLParserOptions, trusted_html_parser_options)

public:
    bool run_scripts() const;
};

}
