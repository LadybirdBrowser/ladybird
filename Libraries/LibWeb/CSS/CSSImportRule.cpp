/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Lorenz Ackermann <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSImportRule.h>

namespace Web::CSS {

// NB: Implemented in Libraries/LibWeb/Bindings/Rust/src/css/exports.rs.
extern "C" void* web_css_import_rule_import(void* rule);

StyleSheetImport& CSSImportRule::import() const
{
    return static_cast<StyleSheetImportCell*>(static_cast<GC::Cell*>(web_css_import_rule_import(const_cast<CSSImportRule*>(this))))->import();
}

}
