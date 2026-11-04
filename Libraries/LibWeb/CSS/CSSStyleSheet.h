/*
 * Copyright (c) 2019-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/CSSStyleSheet.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-1/#cssstylesheet
class WEB_API CSSStyleSheet final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CSSStyleSheet, css_style_sheet)

public:
    [[nodiscard]] static GC::Ref<CSSStyleSheet> create(StyleSheetState&);

    StyleSheetState& state() const;
    void update_owner_chain();
};

}
