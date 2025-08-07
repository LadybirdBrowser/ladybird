/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/CanvasPatternPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/CanvasPattern.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/SVG/SVGImageElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CanvasPattern);

CanvasPattern::CanvasPattern(JS::Realm& realm, CanvasPatternPaintStyle& pattern)
    : PlatformObject(realm)
    , m_pattern(pattern)
{
}

CanvasPattern::~CanvasPattern() = default;

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createpattern
WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> CanvasPattern::create(JS::Realm& realm, CanvasImageSource const& image, StringView repetition)
{
    auto parse_repetition = [&](auto repetition) -> Optional<CanvasPatternPaintStyle::Repetition> {
        if (repetition == "repeat"sv)
            return CanvasPatternPaintStyle::Repetition::Repeat;
        if (repetition == "repeat-x"sv)
            return CanvasPatternPaintStyle::Repetition::RepeatX;
        if (repetition == "repeat-y"sv)
            return CanvasPatternPaintStyle::Repetition::RepeatY;
        if (repetition == "no-repeat"sv)
            return CanvasPatternPaintStyle::Repetition::NoRepeat;
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
    auto pattern = TRY_OR_THROW_OOM(realm.vm(), CanvasPatternPaintStyle::create(image, *repetition_value));

    // FIXME: 7. If image is not origin-clean, then mark pattern as not origin-clean.

    // 8. Return pattern.
    return realm.create<CanvasPattern>(realm, *pattern);
}

void CanvasPattern::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CanvasPattern);
    Base::initialize(realm);
}

}
