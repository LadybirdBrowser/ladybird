/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <PaintServer/Painter.h>

#include <LibGfx/Bitmap.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <LibGfx/VulkanContext.h>
#include <core/SkImage.h>
#include <core/SkRefCnt.h>

class SkImage;
class SkCanvas;

namespace Gfx {

class PaintingSurface;
class SkiaBackendContext;

}

namespace PaintServer {

class VulkanPainter final : public Painter {
public:
    explicit VulkanPainter(PaintingMode painting_mode)
        : Painter(painting_mode)
    {
    }

    virtual ~VulkanPainter() override = default;

private:
    ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> create_gpu_backed_skia_context() override;
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> import_cpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image) override;
    ErrorOr<Gfx::SharedImage> import_gpu_backed_presentation_buffer(Gfx::SharedImagePayload shared_image) override;
    ErrorOr<Gfx::SharedImage> create_gpu_backed_content_image(u64 image_id, Gfx::IntSize size, Gfx::BitmapFormat format) override;
    char const* backend_name() const override;

    static VkFormat to_vk_format(Gfx::BitmapFormat);
};

}
