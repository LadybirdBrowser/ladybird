/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontVariationSettings.h>

struct hb_face_t;

namespace Gfx {

// The weight and the width use the scales of the OS/2 table. The slope is 0 for upright, 1 for italic and 2 for oblique.
struct FaceStyle {
    u16 weight { 400 };
    u16 width { 5 };
    u8 slope { 0 };

    bool operator==(FaceStyle const&) const = default;
};

// Vertical metrics in font units at the default coordinates, with y up.
struct FaceVerticalMetrics {
    i32 ascender { 0 };
    i32 descender { 0 };
    // The sxHeight of the OS/2 table, or 0 if the table does not give it.
    i32 x_height { 0 };
};

// The family, the style and the vertical metrics of a face, read from its tables.
class FaceDescription {
public:
    static FaceDescription read(hb_face_t*);

    FlyString const& family() const { return m_family; }
    FaceStyle style() const { return m_style; }
    FaceVerticalMetrics const& vertical_metrics() const { return m_vertical_metrics; }

    // The style of the face with these variations applied.
    FaceStyle style_for_variations(ReadonlySpan<FontVariationAxis>) const;

private:
    struct VariationAxis {
        u32 tag { 0 };
        float minimum { 0 };
        // The default value, or the value of the named instance that the face index selects.
        float face_value { 0 };
        float maximum { 0 };
    };

    FlyString m_family;
    // The style that the OS/2 and head tables give, which the axes of a variable face can change.
    FaceStyle m_declared_style;
    FaceStyle m_style;
    FaceVerticalMetrics m_vertical_metrics;
    Vector<VariationAxis> m_axes;
};

}
