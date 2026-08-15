/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ContentStyleValue.h"
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

StyleValueFFI::StyleValueData const* ContentStyleValue::make_content_data(ValueComparingNonnullRefPtr<StyleValueList const> const& content, ValueComparingRefPtr<StyleValueList const> const& alt_text)
{
    // The Rust allocation takes ownership of one strong reference to each non-null list data.
    auto const* alt_text_data = alt_text ? StyleValueFFI::rust_style_value_retain(alt_text->rust_style_value_data()) : nullptr;
    return StyleValueFFI::rust_style_value_create_content(StyleValueFFI::rust_style_value_retain(content->rust_style_value_data()), alt_text_data);
}

void ContentStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    const_cast<StyleValueList&>(content()).set_style_sheet(style_sheet);
    if (auto const* alt_text_list = alt_text())
        const_cast<StyleValueList&>(*alt_text_list).set_style_sheet(style_sheet);
}

}
