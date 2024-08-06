/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Painting/GradientPainting.h>

namespace Web::CSS {

// Note: The sides must be before the corners in this enum (as this order is used in parsing).
enum class SideOrCorner {
    Top,
    Bottom,
    Left,
    Right,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

class LinearGradientStyleValue final : public AbstractImageStyleValue {
public:
    using GradientDirection = Variant<Angle, SideOrCorner>;

    enum class GradientType {
        Standard,
        WebKit
    };

    static ValueComparingNonnullRefPtr<LinearGradientStyleValue> create(GradientDirection direction, Vector<LinearColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating)
    {
        VERIFY(color_stop_list.size() >= 2);
        return adopt_ref(*new (nothrow) LinearGradientStyleValue(direction, move(color_stop_list), type, repeating));
    }

    virtual String to_string() const override;
    virtual ~LinearGradientStyleValue() override = default;
    virtual bool equals(StyleValue const& other) const override;

    Vector<LinearColorStopListElement> const& color_stop_list() const
    {
        return m_properties.color_stop_list;
    }

    bool is_repeating() const { return m_properties.repeating == GradientRepeating::Yes; }

    float angle_degrees(CSSPixelSize gradient_size) const;

    void resolve_for_size(Layout::NodeWithStyleAndBoxModelMetrics const&, CSSPixelSize) const override;

    bool is_paintable() const override { return true; }
    void paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering) const override;

private:
    LinearGradientStyleValue(GradientDirection direction, Vector<LinearColorStopListElement> color_stop_list, GradientType type, GradientRepeating repeating)
        : AbstractImageStyleValue(Type::LinearGradient)
        , m_properties { .direction = direction, .color_stop_list = move(color_stop_list), .gradient_type = type, .repeating = repeating }
    {
    }

    struct Properties {
        GradientDirection direction;
        Vector<LinearColorStopListElement> color_stop_list;
        GradientType gradient_type;
        GradientRepeating repeating;
        bool operator==(Properties const&) const = default;
    } m_properties;

    struct ResolvedData {
        Painting::LinearGradientData data;
        CSSPixelSize size;
    };

    mutable Optional<ResolvedData> m_resolved;
};

}
