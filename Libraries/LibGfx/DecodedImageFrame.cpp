/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <RustFFI.h>

extern "C" {
void* ladybird_gfx_decoded_image_frame_retain(void const*, Gfx::FFI::FfiImageFrameSnapshot*);
void ladybird_gfx_decoded_image_frame_release(void*);
}

static_assert(to_underlying(Gfx::BitmapFormat::RGBA8888) == 4);
static_assert(to_underlying(Gfx::AlphaType::Unpremultiplied) == 1);

extern "C" void* ladybird_gfx_decoded_image_frame_retain(void const* frame, Gfx::FFI::FfiImageFrameSnapshot* out_snapshot)
{
    VERIFY(frame);
    VERIFY(out_snapshot);
    auto const& typed_frame = *static_cast<Gfx::DecodedImageFrame const*>(frame);
    *out_snapshot = {
        .id = typed_frame.id(),
        .width = typed_frame.width(),
        .height = typed_frame.height(),
        .bitmap_format_value = to_underlying(typed_frame.format()),
        .alpha_type_value = to_underlying(typed_frame.alpha_type()),
        .pixels = typed_frame.pixels().data(),
        .pitch = typed_frame.pitch(),
    };
    return new Gfx::DecodedImageFrame(typed_frame);
}

extern "C" void ladybird_gfx_decoded_image_frame_release(void* frame)
{
    delete static_cast<Gfx::DecodedImageFrame*>(frame);
}

namespace Gfx {

ErrorOr<DecodedImageFrame> DecodedImageFrame::create_over_shared_memory(Core::AnonymousBuffer shared_memory, size_t offset_of_pixels, IntSize size, BitmapFormat format, AlphaType alpha_type, ColorSpace color_space)
{
    if (!is_valid_bitmap_format(to_underlying(format)))
        return Error::from_string_literal("Gfx::DecodedImageFrame format is invalid");
    if (size.is_empty() || Bitmap::size_would_overflow(format, size))
        return Error::from_string_literal("Gfx::DecodedImageFrame size is invalid");

    auto size_of_pixels = Bitmap::size_in_bytes(Bitmap::minimum_pitch(size.width(), format), size.height());
    if (offset_of_pixels % alignof(RawPixel) != 0 || offset_of_pixels > shared_memory.size() || size_of_pixels > shared_memory.size() - offset_of_pixels)
        return Error::from_string_literal("Gfx::DecodedImageFrame pixels are outside of the shared memory");

    return DecodedImageFrame { move(shared_memory), offset_of_pixels, size, format, alpha_type, move(color_space) };
}

#ifdef AK_OS_MACOS
ErrorOr<DecodedImageFrame> DecodedImageFrame::create_over_iosurface(Core::IOSurfaceHandle iosurface, IntSize size, BitmapFormat format, AlphaType alpha_type, ColorSpace color_space)
{
    TRY(Bitmap::validate_iosurface_holds_pixels_of(iosurface, format, size));
    auto shared_iosurface = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) SharedIOSurface(move(iosurface))));
    return DecodedImageFrame { move(shared_iosurface), size, format, alpha_type, move(color_space) };
}
#endif

ErrorOr<DecodedImageFrame> DecodedImageFrame::create(Bitmap const& bitmap, ColorSpace color_space)
{
#ifdef AK_OS_MACOS
    if (auto const* iosurface = bitmap.iosurface())
        return create_over_iosurface(Core::IOSurfaceHandle::from_ref(iosurface->core_foundation_pointer()), bitmap.size(), bitmap.format(), bitmap.alpha_type(), move(color_space));
#endif
    if (bitmap.anonymous_buffer().is_valid())
        return create_over_shared_memory(bitmap.anonymous_buffer(), 0, bitmap.size(), bitmap.format(), bitmap.alpha_type(), move(color_space));

    if (bitmap.size().is_empty())
        return Error::from_string_literal("Gfx::DecodedImageFrame size is invalid");

    auto bitmap_in_shareable_memory = TRY(bitmap.to_bitmap_shareable_with_gpu_access());
    return create(*bitmap_in_shareable_memory, move(color_space));
}

ErrorOr<DecodedImageFrame> DecodedImageFrame::create_with_id(u64 id, Bitmap const& bitmap, ColorSpace color_space)
{
    auto frame = TRY(create(bitmap, move(color_space)));
    frame.m_id = id;
    return frame;
}

NonnullRefPtr<Bitmap const> DecodedImageFrame::bitmap_sharing_pixels() const
{
#ifdef AK_OS_MACOS
    if (m_iosurface)
        return MUST(Bitmap::create_wrapper(m_format, m_alpha_type, m_size, m_pitch, const_cast<u8*>(m_pixels), [iosurface = m_iosurface] { }));
#endif
    if (m_offset_of_pixels == 0)
        return MUST(Bitmap::create_with_anonymous_buffer(m_format, m_alpha_type, m_shared_memory, m_size));
    return MUST(Bitmap::create_wrapper(m_format, m_alpha_type, m_size, pitch(), const_cast<u8*>(pixels().data()), [shared_memory = m_shared_memory] { }));
}

}

namespace IPC {

enum class DecodedImageFramePixelStorage : u8 {
    SharedMemory,
    IOSurface,
};

static ErrorOr<void> encode_pixel_storage(Encoder& encoder, Gfx::DecodedImageFrame const& frame)
{
#ifdef AK_OS_MACOS
    if (auto const* iosurface = frame.iosurface()) {
        TRY(encoder.encode(to_underlying(DecodedImageFramePixelStorage::IOSurface)));
        TRY(encoder.append_attachment(Attachment::from_mach_port(iosurface->create_mach_port(), Core::MachPort::MessageRight::MoveSend)));
        return {};
    }
#endif
    TRY(encoder.encode(to_underlying(DecodedImageFramePixelStorage::SharedMemory)));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(frame.shared_memory().fd()))));
    TRY(encoder.encode(static_cast<u64>(frame.offset_of_pixels())));
    return {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::DecodedImageFrame const& frame)
{
    TRY(encode_pixel_storage(encoder, frame));
    TRY(encoder.encode(frame.size()));
    TRY(encoder.encode(static_cast<u32>(frame.format())));
    TRY(encoder.encode(static_cast<u32>(frame.alpha_type())));
    TRY(encoder.encode(frame.color_space()));
    return {};
}

template<>
ErrorOr<Gfx::DecodedImageFrame> decode(Decoder& decoder)
{
    Optional<IPC::File> shared_memory_file;
    u64 offset_of_pixels = 0;
#ifdef AK_OS_MACOS
    Optional<Core::IOSurfaceHandle> iosurface;
#endif
    auto pixel_storage = TRY(decoder.decode<UnderlyingType<DecodedImageFramePixelStorage>>());
    if (pixel_storage == to_underlying(DecodedImageFramePixelStorage::IOSurface)) {
#ifdef AK_OS_MACOS
        auto attachment = TRY(decoder.attachments().try_dequeue());
        if (attachment.message_right() != Core::MachPort::MessageRight::MoveSend)
            return Error::from_string_literal("IPC: Gfx::DecodedImageFrame IOSurface attachment is not a moved send right");
        iosurface = TRY(Core::IOSurfaceHandle::from_mach_port(attachment.release_mach_port()));
#else
        return Error::from_string_literal("IPC: This platform has no Gfx::DecodedImageFrame IOSurfaces to decode");
#endif
    } else if (pixel_storage == to_underlying(DecodedImageFramePixelStorage::SharedMemory)) {
        shared_memory_file = TRY(decoder.decode<IPC::File>());
        offset_of_pixels = TRY(decoder.decode<u64>());
    } else {
        return Error::from_string_literal("IPC: Invalid Gfx::DecodedImageFrame pixel storage");
    }

    auto size = TRY(decoder.decode<Gfx::IntSize>());
    if (size.is_empty())
        return Error::from_string_literal("IPC: Invalid Gfx::DecodedImageFrame size");

    auto raw_format = TRY(decoder.decode<u32>());
    if (!Gfx::is_valid_bitmap_format(raw_format))
        return Error::from_string_literal("IPC: Invalid Gfx::DecodedImageFrame format");
    auto format = static_cast<Gfx::BitmapFormat>(raw_format);
    if (Gfx::Bitmap::size_would_overflow(format, size))
        return Error::from_string_literal("IPC: Gfx::DecodedImageFrame size overflow");

    auto raw_alpha_type = TRY(decoder.decode<u32>());
    if (!Gfx::is_valid_alpha_type(raw_alpha_type))
        return Error::from_string_literal("IPC: Invalid Gfx::DecodedImageFrame alpha type");
    auto alpha_type = static_cast<Gfx::AlphaType>(raw_alpha_type);

    auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());

#ifdef AK_OS_MACOS
    if (iosurface.has_value())
        return Gfx::DecodedImageFrame::create_over_iosurface(iosurface.release_value(), size, format, alpha_type, move(color_space));
#endif

    Checked<u64> end_of_pixels = offset_of_pixels;
    end_of_pixels += Gfx::Bitmap::size_in_bytes(Gfx::Bitmap::minimum_pitch(size.width(), format), size.height());
    if (end_of_pixels.has_overflow())
        return Error::from_string_literal("IPC: Gfx::DecodedImageFrame pixels overflow");

    auto shared_memory = TRY(Core::AnonymousBuffer::create_from_anon_fd(shared_memory_file->take_fd(), static_cast<size_t>(end_of_pixels.value())));
    TRY(shared_memory.validate_backing_size());
    return Gfx::DecodedImageFrame::create_over_shared_memory(move(shared_memory), static_cast<size_t>(offset_of_pixels), size, format, alpha_type, move(color_space));
}

}
