/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class OverflowClipMarginStyleValue final : public StyleValueWithDefaultOperators<OverflowClipMarginStyleValue> {
public:
    static ValueComparingNonnullRefPtr<OverflowClipMarginStyleValue const> create(Optional<BackgroundBox> visual_box, NonnullRefPtr<StyleValue const> offset);
    virtual ~OverflowClipMarginStyleValue() override;

    Optional<BackgroundBox> visual_box() const
    {
        if (!m_value->overflow_clip_margin.has_visual_box)
            return {};
        return static_cast<BackgroundBox>(m_value->overflow_clip_margin.visual_box);
    }
    StyleValue const& offset() const { return *static_cast<StyleValue const*>(m_value->overflow_clip_margin.offset.pointer); }

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    bool properties_equal(OverflowClipMarginStyleValue const&) const;

    virtual bool is_computationally_independent() const override { return offset().is_computationally_independent(); }

private:
    OverflowClipMarginStyleValue(Optional<BackgroundBox> visual_box, NonnullRefPtr<StyleValue const> offset);

    RustStyleValueHandle m_value;
};

}
