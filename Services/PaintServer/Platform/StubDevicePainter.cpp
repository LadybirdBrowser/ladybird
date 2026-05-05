/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <PaintServer/Painter.h>

namespace PaintServer {

static Error NOT_SUPPORTED = Error::from_string_literal("Just because it builds doesn't mean it works");

class StubGPUManager final : public Painter {
public:
    explicit StubGPUManager(PaintingMode painting_mode)
        : Painter(painting_mode)
    {
    }

    SubmitResult submit_commands(RenderContext const&, FrameHeader const&, ReadonlyBytes, ReleaseToken) override { return SubmitResult { OperationResult::Completed, {} }; }

private:
    ErrorOr<NonnullRefPtr<Gfx::SkiaBackendContext>> create_gpu_backed_skia_context() override { return NOT_SUPPORTED; }
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> import_cpu_backed_presentation_buffer(Gfx::SharedImagePayload) override { return NOT_SUPPORTED; }
    ErrorOr<Gfx::SharedImage> import_gpu_backed_presentation_buffer(Gfx::SharedImagePayload) override { return NOT_SUPPORTED; }
    ErrorOr<Gfx::SharedImage> create_gpu_backed_content_image(u64, Gfx::IntSize, u32) override { return NOT_SUPPORTED; }
    virtual char const* backend_name() const override { return "StubGPUManager"; }
};

NonnullOwnPtr<Painter> Painter::create(PaintingMode painting_mode)
{
    return make<StubGPUManager>(painting_mode);
}

}
