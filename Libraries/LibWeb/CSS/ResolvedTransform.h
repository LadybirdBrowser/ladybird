/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGfx/Matrix4x4.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

// A transform function pre-lowered into paint-ready data when computed values are
// built, so painting never downcasts style values per frame. Functions whose
// arguments carry percentages (the translate family) keep per-axis pieces that
// resolve against the reference box at paint time; every other function bakes
// straight into a matrix.
class ResolvedTransform {
public:
    struct TranslateAxis {
        float px { 0 };
        // Set when the axis value carries a percentage; resolved against the
        // reference box per paint.
        RefPtr<StyleValue const> percentage_value;
        bool operator==(TranslateAxis const&) const = default;
    };
    struct Translate {
        TranslateAxis x;
        TranslateAxis y;
        float z { 0 };
        bool operator==(Translate const&) const = default;
    };

    ResolvedTransform(Gfx::FloatMatrix4x4 matrix)
        : m_value(matrix)
    {
    }

    ResolvedTransform(Translate translate)
        : m_value(move(translate))
    {
    }

    Gfx::FloatMatrix4x4 to_matrix(CSSPixels reference_width, CSSPixels reference_height) const;

    bool operator==(ResolvedTransform const&) const;

private:
    Variant<Gfx::FloatMatrix4x4, Translate> m_value;
};

}
