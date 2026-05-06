/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <PaintServer/Painter.h>
#include <core/SkImage.h>
#include <core/SkRefCnt.h>

class SkImage;
class SkCanvas;

namespace Gfx {

class SkiaBackendContext;

}

namespace PaintServer {

class MetalPainter final : public Painter {
public:
    explicit MetalPainter(PaintingMode painting_mode)
        : Painter(painting_mode)
    {
    }

    ~MetalPainter() override = default;

private:
    ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> create_gpu_backed_skia_context() override;
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> import_cpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image) override;
    ErrorOr<Gfx::SharedImage> import_gpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image) override;
    ErrorOr<Gfx::SharedImage> create_gpu_backed_content_image(u64 image_id, Gfx::IntSize size, Gfx::BitmapFormat format) override;
    char const* backend_name() const override;
};

}
