/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Color.h>
#include <LibGfx/Cursor.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CursorStyleValue final : public StyleValueWithDefaultOperators<CursorStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CursorStyleValue> create(ValueComparingNonnullRefPtr<AbstractImageStyleValue> image, Optional<NumberOrCalculated> x, Optional<NumberOrCalculated> y)
    {
        VERIFY(x.has_value() == y.has_value());
        return adopt_ref(*new (nothrow) CursorStyleValue(move(image), move(x), move(y)));
    }
    virtual ~CursorStyleValue() override = default;

    ValueComparingNonnullRefPtr<AbstractImageStyleValue> image() const { return m_properties.image; }
    Optional<NumberOrCalculated> const& x() const { return m_properties.x; }
    Optional<NumberOrCalculated> const& y() const { return m_properties.y; }

    Optional<Gfx::ImageCursor> make_image_cursor(Layout::NodeWithStyle const&) const;

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(CursorStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    CursorStyleValue(ValueComparingNonnullRefPtr<AbstractImageStyleValue> image,
        Optional<NumberOrCalculated> x,
        Optional<NumberOrCalculated> y)
        : StyleValueWithDefaultOperators(Type::Cursor)
        , m_properties { .image = move(image), .x = move(x), .y = move(y) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<AbstractImageStyleValue> image;
        Optional<NumberOrCalculated> x;
        Optional<NumberOrCalculated> y;
        bool operator==(Properties const&) const = default;
    } m_properties;

    // Data that can affect the bitmap rendering.
    struct CacheKey {
        Length::ResolutionContext length_resolution_context;
        Gfx::Color current_color;
        bool operator==(CacheKey const&) const = default;
    };
    mutable Optional<CacheKey> m_cache_key;
    mutable Optional<Gfx::ShareableBitmap> m_cached_bitmap;
};

}
