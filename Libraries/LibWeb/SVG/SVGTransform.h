/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::SVG {

// FIXME: This class is just a stub.
// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransform
class SVGTransform final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGTransform, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(SVGTransform);

public:
    [[nodiscard]] static JS::NonnullGCPtr<SVGTransform> create(JS::Realm& realm);
    virtual ~SVGTransform() override;

    enum class Type : u16 {
        Unknown = 0,
        Matrix = 1,
        Translate = 2,
        Scale = 3,
        Rotate = 4,
        SkewX = 5,
        SkewY = 6,
    };

    Type type();
    float angle();

    void set_translate(float tx, float ty);
    void set_scale(float sx, float sy);
    void set_rotate(float angle, float cx, float cy);
    void set_skew_x(float angle);
    void set_skew_y(float angle);

private:
    SVGTransform(JS::Realm& realm);

    virtual void initialize(JS::Realm& realm) override;
};

}
