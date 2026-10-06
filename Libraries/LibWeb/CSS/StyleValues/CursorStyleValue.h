/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Color.h>
#include <LibGfx/Cursor.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class CursorStyleValue final : public StyleValueWithDefaultOperators<CursorStyleValue> {
public:
    virtual ~CursorStyleValue() override = default;

    AbstractImageStyleValue const& image() const { return m_image; }

    Optional<Gfx::ImageCursor> make_image_cursor(Layout::NodeWithStyle const&, GC::Ptr<HTML::DecodedImageData>) const;

private:
    friend class StyleValue;

    explicit CursorStyleValue(StyleValueFFI::StyleValueData const*);

    ValueComparingRefPtr<StyleValue const> x() const { return wrap_rust_child_or_null(m_value->cursor.x); }
    ValueComparingRefPtr<StyleValue const> y() const { return wrap_rust_child_or_null(m_value->cursor.y); }

    // NB: The image wrapper stays a member: image style values carry C++-side loading state
    // (clients, resource requests) keyed on wrapper identity.
    ValueComparingNonnullRefPtr<AbstractImageStyleValue const> m_image;

    mutable Optional<Color> m_cached_bitmap_color;
    mutable Optional<PreferredColorScheme> m_cached_bitmap_color_scheme;
    mutable Optional<Gfx::ShareableBitmap> m_cached_bitmap;
};

}
