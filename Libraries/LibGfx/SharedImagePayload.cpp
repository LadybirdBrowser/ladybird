/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkImage.h>

#ifdef AK_OS_MACOS
static Core::MachPort copy_send_right(Core::MachPort const& port)
{
    auto result = mach_port_mod_refs(mach_task_self(), port.port(), MACH_PORT_RIGHT_SEND, +1);
    VERIFY(result == KERN_SUCCESS);
    return Core::MachPort::adopt_right(port.port(), Core::MachPort::PortRight::Send);
}
#endif

namespace Gfx {

u32 SharedImagePayload::row_bytes() const
{
    return m_info.row_bytes;
}

ErrorOr<void> upload_decoded_image_frame_to_bitmap(DecodedImageFrame const& source, Bitmap& destination, BitmapInfo const& destination_info, ColorSpace const& destination_color_space)
{
    if (destination.size() != source.size())
        return Error::from_string_literal("SharedImagePayload: destination_size_mismatch");
    if (destination_info.size != source.size())
        return Error::from_string_literal("SharedImagePayload: destination_info_size_mismatch");
    if (destination.format() != destination_info.pixel_format)
        return Error::from_string_literal("SharedImagePayload: destination_format_mismatch");
    if (destination.alpha_type() != destination_info.alpha_type)
        return Error::from_string_literal("SharedImagePayload: destination_alpha_type_mismatch");

    auto const& source_bitmap = source.bitmap();
    bool color_space_matches = false;
    if (destination_info.color_space == BitmapColorSpace::SRGB) {
        color_space_matches = !source.color_space().is_valid() || source.color_space().is_srgb();
    } else if (destination_info.color_space == BitmapColorSpace::Linear) {
        color_space_matches = source.color_space().is_linear();
    }
    if (color_space_matches && source_bitmap.format() == destination.format() && source_bitmap.alpha_type() == destination.alpha_type()) {
        size_t bytes_per_row = Bitmap::minimum_pitch(source_bitmap.width(), source_bitmap.format());
        if (source_bitmap.pitch() == destination.pitch()) {
            __builtin_memcpy(destination.begin(), source_bitmap.begin(), source_bitmap.size_in_bytes());
            return {};
        }
        for (int y = 0; y < source.height(); ++y)
            __builtin_memcpy(destination.scanline_u8(y), source_bitmap.scanline_u8(y), bytes_per_row);
        return {};
    }

    auto sk_image = sk_image_from_bitmap(source_bitmap, source.color_space());
    if (!sk_image)
        return Error::from_string_literal("SharedImagePayload: missing_source_image");

    NonnullRefPtr<PaintingSurface> painting_surface = PaintingSurface::wrap_bitmap(destination, destination_color_space, destination_info.color_space);
    SkCanvas& canvas = painting_surface->canvas();
    canvas.clear(SK_ColorTRANSPARENT);
    canvas.drawImage(sk_image, 0.0f, 0.0f);

    return {};
}

#ifdef AK_OS_MACOS
SharedImagePayload::SharedImagePayload(BitmapInfo description, ShareableBitmap shareable_bitmap, ColorSpace color_space)
    : m_info(description)
    , m_color_space(move(color_space))
    , m_data(move(shareable_bitmap))
{
}

SharedImagePayload::SharedImagePayload(BitmapInfo description, Core::MachPort&& port, ColorSpace color_space)
    : m_info(description)
    , m_color_space(move(color_space))
    , m_data(move(port))
{
}
#else
SharedImagePayload::SharedImagePayload(BitmapInfo description, ShareableBitmap shareable_bitmap, ColorSpace color_space)
    : m_info(description)
    , m_color_space(move(color_space))
    , m_data(move(shareable_bitmap))
{
}

SharedImagePayload::SharedImagePayload(BitmapInfo description, LinuxDmaBufPayload&& dmabuf, ColorSpace color_space)
    : m_info(description)
    , m_color_space(move(color_space))
    , m_data(move(dmabuf))
{
}

#    ifndef USE_VULKAN_DMABUF_IMAGES
ErrorOr<void> upload_decoded_image_frame_to_shared_image(DecodedImageFrame const& frame, SharedImagePayload& payload)
{
    if (auto* shareable_bitmap = payload.shareable_bitmap()) {
        auto* destination_bitmap = shareable_bitmap->bitmap();
        if (!destination_bitmap)
            return Error::from_string_literal("SharedImagePayload: shareable_bitmap_missing_destination_bitmap");
        return upload_decoded_image_frame_to_bitmap(frame, *destination_bitmap, payload.info(), payload.color_space());
    }

    (void)payload;
    return Error::from_string_literal("SharedImagePayload: unsupported_backing_kind");
}

#    endif
#endif

}

namespace IPC {

enum class SharedImageBackingType : u8 {
    ShareableBitmap,
#ifdef AK_OS_MACOS
    MachPort,
#else
    LinuxDmaBuf,
#endif
};

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::LinuxDmaBufPayload const& dmabuf)
{
    TRY(encoder.encode(dmabuf.drm_format));
    TRY(encoder.encode(dmabuf.stride));
    TRY(encoder.encode(dmabuf.offset));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(dmabuf.file.fd()))));
    return {};
}

template<>
ErrorOr<Gfx::LinuxDmaBufPayload> decode(Decoder& decoder)
{
    return Gfx::LinuxDmaBufPayload {
        .drm_format = TRY(decoder.decode<u32>()),
        .stride = TRY(decoder.decode<u32>()),
        .offset = TRY(decoder.decode<u32>()),
        .file = TRY(decoder.decode<IPC::File>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SharedImagePayload const& payload)
{
    TRY(encoder.encode(payload.m_info));
    TRY(encoder.encode(payload.m_color_space));
#ifdef AK_OS_MACOS
    return payload.m_data.visit(
        [&](Gfx::ShareableBitmap const& shareable_bitmap) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::ShareableBitmap));
            TRY(encoder.encode(shareable_bitmap));
            return {};
        },
        [&](Core::MachPort const& port) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::MachPort));
            TRY(encoder.append_attachment(Attachment::from_mach_port(copy_send_right(port), Core::MachPort::MessageRight::MoveSend)));
            return {};
        });
#else
    return payload.m_data.visit(
        [&](Gfx::ShareableBitmap const& shareable_bitmap) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::ShareableBitmap));
            TRY(encoder.encode(shareable_bitmap));
            return {};
        },
        [&](Gfx::LinuxDmaBufPayload const& dmabuf) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::LinuxDmaBuf));
            TRY(encoder.encode(dmabuf));
            return {};
        });
#endif
    return {};
}

template<>
ErrorOr<Gfx::SharedImagePayload> decode(Decoder& decoder)
{
    auto description = TRY(decoder.decode<Gfx::BitmapInfo>());
    auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());
#ifdef AK_OS_MACOS
    switch (TRY(decoder.decode<SharedImageBackingType>())) {
    case SharedImageBackingType::ShareableBitmap:
        return Gfx::SharedImagePayload { description, TRY(decoder.decode<Gfx::ShareableBitmap>()), move(color_space) };
    case SharedImageBackingType::MachPort: {
        auto attachment = decoder.attachments().dequeue();
        VERIFY(attachment.message_right() == Core::MachPort::MessageRight::MoveSend);
        return Gfx::SharedImagePayload { description, attachment.release_mach_port(), move(color_space) };
    }
    default:
        VERIFY_NOT_REACHED();
    }
#else
    switch (TRY(decoder.decode<SharedImageBackingType>())) {
    case SharedImageBackingType::ShareableBitmap:
        return Gfx::SharedImagePayload { description, TRY(decoder.decode<Gfx::ShareableBitmap>()), move(color_space) };
    case SharedImageBackingType::LinuxDmaBuf:
        return Gfx::SharedImagePayload { description, TRY(decoder.decode<Gfx::LinuxDmaBufPayload>()), move(color_space) };
    default:
        VERIFY_NOT_REACHED();
    }
#endif
}

}
