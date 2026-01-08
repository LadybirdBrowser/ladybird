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

void ContentStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_properties.content->serialize(builder, mode);
    if (has_alt_text()) {
        builder.append(" / "sv);
        m_properties.alt_text->serialize(builder, mode);
    }
}

void ContentStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);
    const_cast<StyleValueList&>(*m_properties.content).set_style_sheet(style_sheet);
    if (m_properties.alt_text)
        const_cast<StyleValueList&>(*m_properties.alt_text).set_style_sheet(style_sheet);
}

}
