/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Layout::RustFFI {

struct DocumentHost;

}

namespace Web::Painting {

// What an SVG image is rendered for: two renderings for one key show the same.
struct VectorImageKey {
    CSSPixelSize css_size;
    float raster_scale { 1 };
    CSS::PreferredColorScheme color_scheme;
    HTML::PaintConfig paint_config;
};

struct ImagePaint {
    struct DecodedFrame {
        Gfx::DecodedImageFrame frame;
        Gfx::IntSize natural_size;
    };
    // An SVG image, which the host of its document keeps rendered for the key.
    struct VectorImage {
        Layout::RustFFI::DocumentHost const* host;
        VectorImageKey key;
        Gfx::IntSize list_size;
    };
    struct Gradient {
        NonnullRefPtr<CSS::StyleValue const> style_value;
    };
    Variant<DecodedFrame, VectorImage, Gradient> value;
};

struct ImagePaintRequest {
    GC::Ref<DOM::Document const> document;
    Gfx::FloatRect dest_rect;
    CSS::ImageRendering image_rendering;
    CSS::PreferredColorScheme color_scheme;
    CSS::ColorResolutionStyle gradient_stop_color_resolution_style;
    Gfx::FloatSize accumulated_scale;
};

}
