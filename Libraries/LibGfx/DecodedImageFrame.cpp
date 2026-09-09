/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <RustFFI.h>

extern "C" {
void* ladybird_gfx_decoded_image_frame_retain(void const*, Gfx::FFI::FfiImageFrameSnapshot*);
void ladybird_gfx_decoded_image_frame_release(void*);
}

extern "C" void* ladybird_gfx_decoded_image_frame_retain(void const* frame, Gfx::FFI::FfiImageFrameSnapshot* out_snapshot)
{
    VERIFY(frame);
    VERIFY(out_snapshot);
    auto const& typed_frame = *static_cast<Gfx::DecodedImageFrame const*>(frame);
    *out_snapshot = {
        .id = typed_frame.id(),
        .width = typed_frame.width(),
        .height = typed_frame.height(),
    };
    return new Gfx::DecodedImageFrame(typed_frame);
}

extern "C" void ladybird_gfx_decoded_image_frame_release(void* frame)
{
    delete static_cast<Gfx::DecodedImageFrame*>(frame);
}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::DecodedImageFrame const& frame)
{
    auto bitmap = frame.bitmap().to_shareable_bitmap();
    TRY(encoder.encode(bitmap));
    TRY(encoder.encode(frame.color_space()));
    return {};
}

template<>
ErrorOr<Gfx::DecodedImageFrame> decode(Decoder& decoder)
{
    auto bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());
    return Gfx::DecodedImageFrame { *bitmap.bitmap(), move(color_space) };
}

}
