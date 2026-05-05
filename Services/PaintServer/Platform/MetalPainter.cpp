/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/MetalContext.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibPaintServer/Debug.h>
#include <PaintServer/Platform/MetalPainter.h>
#include <core/SkImage.h>

namespace PaintServer {

ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> MetalPainter::create_gpu_backed_skia_context()
{
    auto metal_context = Gfx::get_metal_context();
    if (!metal_context)
        return Error::from_string_literal("Failed to create Metal context");

    auto context = Gfx::SkiaBackendContext::create_metal_context(metal_context.release_nonnull());
    if (!context)
        return Error::from_string_literal("Failed to create Skia backend context");

    return context.release_nonnull();
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> MetalPainter::import_cpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image)
{
    auto imported_shared_image = Gfx::SharedImage::import_from_payload(move(shared_image));
    return imported_shared_image.bitmap();
}

ErrorOr<Gfx::SharedImage> MetalPainter::import_gpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image)
{
    return Gfx::SharedImage::import_from_payload(move(shared_image));
}

ErrorOr<Gfx::SharedImage> MetalPainter::create_gpu_backed_content_image(u64 image_id, Gfx::IntSize size, Gfx::BitmapFormat format)
{
    (void)image_id;

    Gfx::BitmapInfo image_descriptor {
        .size = size,
        .row_bytes = 0,
        .mip_level_count = 1,
        .sample_count = 1,
        .tiling_modifier = 0,
        .pixel_format = format,
        .color_space = Gfx::BitmapColorSpace::SRGB,
        .alpha_type = Gfx::BitmapAlpha::Premultiplied,
        .origin = Gfx::BitmapOrigin::TopLeft,
    };

    return Gfx::SharedImage::create(image_descriptor);
}

char const* MetalPainter::backend_name() const
{
    return "MetalPainter";
}

NonnullOwnPtr<Painter> Painter::create(PaintingMode painting_mode)
{
    return make<MetalPainter>(painting_mode);
}

}
