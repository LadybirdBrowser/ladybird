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
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct ImagePaint {
    struct DecodedFrame {
        Gfx::DecodedImageFrame frame;
        Gfx::IntSize natural_size;
    };
    struct NestedDisplayList {
        DisplayListResource resource;
        Gfx::IntSize list_size;
    };
    Variant<DecodedFrame, NestedDisplayList, LinearGradientData, ResolvedRadialGradient, ResolvedConicGradient> value;
};

struct ImagePaintRequest {
    GC::Ref<DOM::Document const> document;
    Gfx::FloatRect dest_rect;
    CSS::ImageRendering image_rendering;
    CSS::PreferredColorScheme color_scheme;
    Gfx::FloatSize accumulated_scale;
    DisplayListResourceStorage& resource_storage;
};

}
