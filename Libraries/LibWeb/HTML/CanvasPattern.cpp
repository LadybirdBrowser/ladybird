/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CanvasPatternPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/CanvasPattern.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CanvasPattern);

CanvasPattern::CanvasPattern(JS::Realm& realm, Gfx::CanvasPatternPaintStyle& pattern)
    : PlatformObject(realm)
    , m_pattern(pattern)
{
}

CanvasPattern::~CanvasPattern() = default;

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createpattern
WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> CanvasPattern::create(JS::Realm& realm, CanvasImageSource const& image, StringView repetition)
{
    auto parse_repetition = [&](auto value) -> Optional<Gfx::CanvasPatternPaintStyle::Repetition> {
        if (value == "repeat"sv)
            return Gfx::CanvasPatternPaintStyle::Repetition::Repeat;
        if (value == "repeat-x"sv)
            return Gfx::CanvasPatternPaintStyle::Repetition::RepeatX;
        if (value == "repeat-y"sv)
            return Gfx::CanvasPatternPaintStyle::Repetition::RepeatY;
        if (value == "no-repeat"sv)
            return Gfx::CanvasPatternPaintStyle::Repetition::NoRepeat;
        return {};
    };

    // 1. Let usability be the result of checking the usability of image.
    auto usability = TRY(check_usability_of_image(image));

    // 2. If usability is bad, then return null.
    if (usability == CanvasImageSourceUsability::Bad)
        return GC::Ptr<CanvasPattern> {};

    // 3. Assert: usability is good.
    VERIFY(usability == CanvasImageSourceUsability::Good);

    // 4. If repetition is the empty string, then set it to "repeat".
    if (repetition.is_empty())
        repetition = "repeat"sv;

    // 5. If repetition is not identical to one of "repeat", "repeat-x", "repeat-y", or "no-repeat",
    // then throw a "SyntaxError" DOMException.
    auto repetition_value = parse_repetition(repetition);
    if (!repetition_value.has_value())
        return WebIDL::SyntaxError::create(realm, "Repetition value is not valid"_utf16);

    // 6. Let pattern be a new CanvasPattern object with the image image and the repetition behavior given by repetition.
    auto immutable_bitmap = canvas_image_source_bitmap(image);
    auto paint_style = TRY_OR_THROW_OOM(realm.vm(), Gfx::CanvasPatternPaintStyle::create(immutable_bitmap, *repetition_value));
    auto pattern = realm.create<CanvasPattern>(realm, *paint_style);

    // FIXME: 7. If image is not origin-clean, then mark pattern as not origin-clean.

    // 8. Return pattern.
    return pattern;
}

void CanvasPattern::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CanvasPattern);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-canvaspattern-settransform
WebIDL::ExceptionOr<void> CanvasPattern::set_transform(Geometry::DOMMatrix2DInit& transform)
{
    // 1. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
    auto matrix = TRY(Geometry::DOMMatrix::create_from_dom_matrix_2d_init(realm(), transform));

    // 2. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42 element are infinite or NaN, then return.
    if (!isfinite(matrix->m11()) || !isfinite(matrix->m12()) || !isfinite(matrix->m21()) || !isfinite(matrix->m22()) || !isfinite(matrix->m41()) || !isfinite(matrix->m42()))
        return {};

    // 3. Reset the pattern's transformation matrix to matrix.
    Gfx::AffineTransform affine_transform(matrix->a(), matrix->b(), matrix->c(), matrix->d(), matrix->e(), matrix->f());
    m_pattern->set_transform(affine_transform);

    return {};
}

}
