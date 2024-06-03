/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DeprecatedFlyString.h>
#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/VectorFont.h>

namespace Gfx {

class Typeface : public RefCounted<Typeface> {
public:
    Typeface(FlyString family, FlyString variant)
        : m_family(move(family))
        , m_variant(move(variant))
    {
    }

    FlyString const& family() const { return m_family; }
    FlyString const& variant() const { return m_variant; }
    unsigned weight() const;
    unsigned width() const;
    u8 slope() const;

    bool is_fixed_width() const;

    void set_vector_font(RefPtr<VectorFont>);

    RefPtr<Font> get_font(float point_size) const;

private:
    FlyString m_family;
    FlyString m_variant;

    RefPtr<VectorFont> m_vector_font;
};

}
