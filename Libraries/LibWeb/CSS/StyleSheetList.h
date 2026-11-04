/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/StyleSheetList.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#the-stylesheetlist-interface
class WEB_API StyleSheetList final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(StyleSheetList, style_sheet_list)

public:
    [[nodiscard]] static GC::Ref<StyleSheetList> create(StyleScope&);
};

}
