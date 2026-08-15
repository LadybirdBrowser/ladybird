/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Filter.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

class Filter {
public:
    // Plain, paint-ready filter operations, lowered once when computed values are
    // built so painting never downcasts style values per frame.
    struct Blur {
        float resolved_radius { 0 };
        bool operator==(Blur const&) const = default;
    };
    struct DropShadow {
        CSSPixels offset_x;
        CSSPixels offset_y;
        CSSPixels radius;
        Gfx::Color color;
        bool operator==(DropShadow const&) const = default;
    };
    struct ColorOperation {
        Gfx::ColorFilterType operation;
        float resolved_amount { 0 };
        bool operator==(ColorOperation const&) const = default;
    };
    struct HueRotate {
        float angle_degrees { 0 };
        bool operator==(HueRotate const&) const = default;
    };
    struct Url {
        // Pre-extracted fragment of a url(#fragment) filter reference; empty when the
        // reference cannot name an SVG filter element.
        Utf16String fragment;
        bool operator==(Url const&) const = default;
    };
    using FilterOperation = Variant<Blur, DropShadow, ColorOperation, HueRotate, Url>;

    Filter() = default;
    Filter(StyleValueList const& filter_value_list)
        : m_filter_value_list { filter_value_list }
    {
    }

    static Filter make_none()
    {
        return Filter {};
    }

    // Builds a filter whose paint-ready operations were already lowered by
    // the core; a null list is the none value.
    static Filter create_lowered(RefPtr<StyleValueList const> filter_value_list, Vector<FilterOperation> operations)
    {
        Filter filter;
        filter.m_filter_value_list = move(filter_value_list);
        filter.m_operations = move(operations);
        return filter;
    }

    bool has_filters() const { return m_filter_value_list; }
    bool is_none() const { return !has_filters(); }

    Vector<FilterOperation> const& operations() const { return m_operations; }

    bool operator==(Filter const&) const = default;

private:
    RefPtr<StyleValueList const> m_filter_value_list { nullptr };
    Vector<FilterOperation> m_operations;
};

}
