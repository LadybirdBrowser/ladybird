/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Color.h>
#include <LibGfx/Cursor.h>
#include <LibWeb/CSS/CalculatedOr.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CursorStyleValue final : public StyleValueWithDefaultOperators<CursorStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CursorStyleValue const> create(ValueComparingNonnullRefPtr<AbstractImageStyleValue const> image, RefPtr<StyleValue const> x, RefPtr<StyleValue const> y)
    {
        // We require either both or neither the X and Y parameters
        VERIFY((!x && !y) || (x && y));
        return adopt_ref(*new (nothrow) CursorStyleValue(move(image), move(x), move(y)));
    }
    virtual ~CursorStyleValue() override = default;

    Optional<Gfx::ImageCursor> make_image_cursor(Layout::NodeWithStyle const&) const;

    virtual String to_string(SerializationMode) const override;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(CursorStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    CursorStyleValue(ValueComparingNonnullRefPtr<AbstractImageStyleValue const> image,
        RefPtr<StyleValue const> x,
        RefPtr<StyleValue const> y)
        : StyleValueWithDefaultOperators(Type::Cursor)
        , m_properties { .image = move(image), .x = move(x), .y = move(y) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<AbstractImageStyleValue const> image;
        RefPtr<StyleValue const> x;
        RefPtr<StyleValue const> y;
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
