/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGC/Ptr.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

struct ImagePaint {
    struct DecodedFrame {
        Gfx::DecodedImageFrame frame;
        Gfx::IntSize natural_size;
    };
    struct NestedDisplayList {
        Compositing::DisplayListResource resource;
        Gfx::IntSize list_size;
    };
    struct Gradient {
        NonnullRefPtr<CSS::StyleValue const> style_value;
    };
    Variant<DecodedFrame, NestedDisplayList, Gradient> value;
};

struct ImagePaintRequest {
    GC::Ref<DOM::Document const> document;
    Gfx::FloatRect dest_rect;
    CSS::ImageRendering image_rendering;
    CSS::PreferredColorScheme color_scheme;
    CSS::ColorResolutionContext gradient_stop_color_resolution_context;
    Gfx::FloatSize accumulated_scale;
    Compositing::DisplayListResourceStorage& resource_storage;
};

}
