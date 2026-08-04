/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class WEB_API OverflowClipMarginStyleValue final : public StyleValueWithDefaultOperators<OverflowClipMarginStyleValue> {
public:
    static ValueComparingNonnullRefPtr<OverflowClipMarginStyleValue const> create(Optional<BackgroundBox> visual_box, NonnullRefPtr<StyleValue const> offset);
    virtual ~OverflowClipMarginStyleValue() override;

    Optional<BackgroundBox> visual_box() const
    {
        if (!m_value->overflow_clip_margin.has_visual_box)
            return {};
        return static_cast<BackgroundBox>(m_value->overflow_clip_margin.visual_box);
    }
    StyleValue const& offset() const { return *m_offset; }

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool properties_equal(OverflowClipMarginStyleValue const&) const;

private:
    friend class StyleValue;

    explicit OverflowClipMarginStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::OverflowClipMargin, data)
        , m_offset(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->overflow_clip_margin.offset.pointer))))
    {
    }

    OverflowClipMarginStyleValue(Optional<BackgroundBox> visual_box, NonnullRefPtr<StyleValue const> offset);

    ValueComparingNonnullRefPtr<StyleValue const> m_offset;
};

}
