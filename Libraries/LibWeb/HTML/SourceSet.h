/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/CalculatedOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/images.html#image-source
struct ImageSource {
    struct PixelDensityDescriptorValue {
        double value { 0 };
    };

    struct WidthDescriptorValue {
        CSSPixels value { 0 };
    };

    String url;
    Variant<Empty, PixelDensityDescriptorValue, WidthDescriptorValue> descriptor;
};

struct ImageSourceAndPixelDensity {
    ImageSource source;
    double pixel_density { 1.0f };
};

// https://html.spec.whatwg.org/multipage/images.html#source-set
struct SourceSet {
    static SourceSet create(DOM::Element const& element, String const& default_source, String const& srcset, String const& sizes, HTML::HTMLImageElement const* img = nullptr);

    [[nodiscard]] bool is_empty() const;

    // https://html.spec.whatwg.org/multipage/images.html#select-an-image-source-from-a-source-set
    [[nodiscard]] ImageSourceAndPixelDensity select_an_image_source();

    // https://html.spec.whatwg.org/multipage/images.html#normalise-the-source-densities
    void normalize_source_densities(DOM::Element const&);

    SourceSet();

    Vector<ImageSource> m_sources;
    CSS::LengthOrCalculated m_source_size;
};

SourceSet parse_a_srcset_attribute(StringView);
[[nodiscard]] CSS::LengthOrCalculated parse_a_sizes_attribute(DOM::Element const& element, StringView sizes, HTML::HTMLImageElement const* img = nullptr);

}
