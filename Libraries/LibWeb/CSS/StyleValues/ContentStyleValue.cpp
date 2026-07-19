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

StyleValueFFI::StyleValueData* ContentStyleValue::make_content_data(ValueComparingNonnullRefPtr<StyleValueList const> content, ValueComparingRefPtr<StyleValueList const> const& alt_text)
{
    // The Rust allocation takes ownership of one strong reference to each non-null list.
    if (alt_text)
        alt_text->ref();
    return StyleValueFFI::rust_style_value_create_content(&content.leak_ref(), alt_text.ptr());
}

bool ContentStyleValue::properties_equal(ContentStyleValue const& other) const
{
    auto lists_equal = [](StyleValueList const* a, StyleValueList const* b) {
        if (!a || !b)
            return a == b;
        return a->equals(*b);
    };
    return content().equals(other.content()) && lists_equal(alt_text(), other.alt_text());
}

void ContentStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    content().serialize(builder, mode);
    if (auto const* alt_text_list = alt_text()) {
        builder.append(" / "sv);
        alt_text_list->serialize(builder, mode);
    }
}

void ContentStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    const_cast<StyleValueList&>(content()).set_style_sheet(style_sheet);
    if (auto const* alt_text_list = alt_text())
        const_cast<StyleValueList&>(*alt_text_list).set_style_sheet(style_sheet);
}

}
