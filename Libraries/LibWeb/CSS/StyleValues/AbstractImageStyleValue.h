/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/ImagePaint.h>

namespace Web::CSS {

// An image value. A gradient is one as it is: Rust reads it and paints it.
class WEB_API AbstractImageStyleValue : public StyleValue {
public:
    using StyleValue::StyleValue;

    virtual void load_any_resources(DOM::Document&) { }
    virtual void load_any_resources(Layout::NodeWithStyle const&);

    virtual bool is_paintable(GC::Ptr<HTML::DecodedImageData>) const { return true; }
    virtual SizeWithAspectRatio natural_size(HTML::DecodedImageData const&) const;
    virtual Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const;

    ImageStyleValue const* selected_image_style_value() const;

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;
};

}
