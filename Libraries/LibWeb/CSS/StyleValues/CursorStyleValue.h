/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Color.h>
#include <LibGfx/Cursor.h>
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

    AbstractImageStyleValue const& image() const { return *static_cast<AbstractImageStyleValue const*>(m_value->cursor.image.pointer); }

    Optional<Gfx::ImageCursor> make_image_cursor(Layout::NodeWithStyle const&) const;

    void serialize(StringBuilder&, SerializationMode) const;

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(CursorStyleValue const& other) const { return image_as_style_value().equals(other.image_as_style_value()) && x() == other.x() && y() == other.y(); }

private:
    CursorStyleValue(ValueComparingNonnullRefPtr<AbstractImageStyleValue const> image,
        RefPtr<StyleValue const> x,
        RefPtr<StyleValue const> y)
        : StyleValueWithDefaultOperators(Type::Cursor, make_cursor_data(image, x, y))
    {
    }

    static StyleValueFFI::StyleValueData* make_cursor_data(NonnullRefPtr<AbstractImageStyleValue const> const&, RefPtr<StyleValue const> const&, RefPtr<StyleValue const> const&);

    StyleValue const& image_as_style_value() const { return *static_cast<StyleValue const*>(m_value->cursor.image.pointer); }

    ValueComparingRefPtr<StyleValue const> x() const { return static_cast<StyleValue const*>(m_value->cursor.x.pointer); }
    ValueComparingRefPtr<StyleValue const> y() const { return static_cast<StyleValue const*>(m_value->cursor.y.pointer); }

    mutable Optional<Color> m_cached_bitmap_color;
    mutable Optional<Gfx::ShareableBitmap> m_cached_bitmap;
};

}
