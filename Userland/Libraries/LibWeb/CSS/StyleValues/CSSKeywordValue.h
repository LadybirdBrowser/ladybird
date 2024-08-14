/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/ValueID.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#csskeywordvalue
class CSSKeywordValue final : public StyleValueWithDefaultOperators<CSSKeywordValue> {
public:
    static ValueComparingNonnullRefPtr<CSSKeywordValue> create(ValueID id)
    {
        return adopt_ref(*new (nothrow) CSSKeywordValue(id));
    }
    virtual ~CSSKeywordValue() override = default;

    ValueID id() const { return m_id; }

    static bool is_color(ValueID);
    virtual bool has_color() const override;
    virtual Color to_color(Optional<Layout::NodeWithStyle const&> node) const override;
    virtual String to_string() const override;

    bool properties_equal(CSSKeywordValue const& other) const { return m_id == other.m_id; }

private:
    explicit CSSKeywordValue(ValueID id)
        : StyleValueWithDefaultOperators(Type::Keyword)
        , m_id(id)
    {
    }

    ValueID m_id { ValueID::Invalid };
};

}
