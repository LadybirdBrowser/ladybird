/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ContentStyleValue final : public StyleValueWithDefaultOperators<ContentStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ContentStyleValue const> create(ValueComparingNonnullRefPtr<StyleValueList const> content, ValueComparingRefPtr<StyleValueList const> alt_text)
    {
        return adopt_ref(*new (nothrow) ContentStyleValue(move(content), move(alt_text)));
    }
    virtual ~ContentStyleValue() override = default;

    StyleValueList const& content() const { return *static_cast<StyleValueList const*>(m_value->content.content.pointer); }
    StyleValueList const* alt_text() const { return static_cast<StyleValueList const*>(m_value->content.alt_text.pointer); }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(ContentStyleValue const& other) const;

    void set_style_sheet(GC::Ptr<CSSStyleSheet>);

private:
    ContentStyleValue(ValueComparingNonnullRefPtr<StyleValueList const> content, ValueComparingRefPtr<StyleValueList const> alt_text)
        : StyleValueWithDefaultOperators(Type::Content, make_content_data(move(content), alt_text))
    {
    }

    static StyleValueFFI::StyleValueData* make_content_data(ValueComparingNonnullRefPtr<StyleValueList const>, ValueComparingRefPtr<StyleValueList const> const&);
};

}
