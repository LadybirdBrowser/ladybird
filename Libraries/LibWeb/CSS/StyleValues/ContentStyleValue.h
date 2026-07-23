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

    StyleValueList const& content() const { return m_content; }
    StyleValueList const* alt_text() const { return m_alt_text.ptr(); }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(ContentStyleValue const& other) const;

    void set_style_sheet(GC::Ptr<CSSStyleSheet>);

private:
    friend class StyleValue;

    ContentStyleValue(ValueComparingNonnullRefPtr<StyleValueList const> content, ValueComparingRefPtr<StyleValueList const> alt_text)
        : StyleValueWithDefaultOperators(Type::Content, make_content_data(content, alt_text))
        , m_content(move(content))
        , m_alt_text(move(alt_text))
    {
    }

    explicit ContentStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::Content, data)
        , m_content(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->content.content.pointer)))->as_value_list())
    {
        auto const* alt_text_data = static_cast<StyleValueFFI::StyleValueData const*>(data->content.alt_text.pointer);
        if (alt_text_data)
            m_alt_text = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(alt_text_data))->as_value_list();
    }

    static StyleValueFFI::StyleValueData const* make_content_data(ValueComparingNonnullRefPtr<StyleValueList const> const&, ValueComparingRefPtr<StyleValueList const> const&);

    ValueComparingNonnullRefPtr<StyleValueList const> m_content;
    ValueComparingRefPtr<StyleValueList const> m_alt_text;
};

}
