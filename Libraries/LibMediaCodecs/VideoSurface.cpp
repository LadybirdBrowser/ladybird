/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/VideoSurface.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/VideoSurfaceImage.h>
#endif

namespace Media {

VideoSurface::~VideoSurface() = default;

Gfx::IntSize VideoSurface::size() const
{
#if defined(AK_OS_MACOS)
    return { static_cast<int>(m_io_surface.plane_width(0)), static_cast<int>(m_io_surface.plane_height(0)) };
#elif defined(AK_OS_LINUX)
    return m_dma_buf.size;
#else
    return {};
#endif
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VideoSurface::read_back_bitmap([[maybe_unused]] CodingIndependentCodePoints const& cicp) const
{
#ifdef AK_OS_MACOS
    return Gfx::bitmap_from_video_surface(m_io_surface, cicp);
#else
    return Error::from_string_literal("This process cannot read the video surface's pixels back");
#endif
}

#ifdef AK_OS_MACOS

ErrorOr<NonnullRefPtr<VideoSurface>> VideoSurface::create(Core::IOSurfaceHandle io_surface)
{
    auto id = io_surface.id();
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoSurface(move(io_surface), id));
}

ErrorOr<NonnullRefPtr<VideoSurface>> VideoSurface::create_from_mach_port(Core::MachPort const& port)
{
    auto io_surface = TRY(Core::IOSurfaceHandle::from_mach_port(port));
    TRY(Gfx::validate_biplanar_video_surface(io_surface));
    return create(move(io_surface));
}

#endif

#ifdef AK_OS_LINUX

ErrorOr<NonnullRefPtr<VideoSurface>> VideoSurface::create_from_dma_buf(u64 id, Gfx::LinuxDmaBufHandle dma_buf)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoSurface(id, move(dma_buf)));
}

#endif

}

namespace IPC {

template<>
ErrorOr<void> encode([[maybe_unused]] Encoder& encoder, [[maybe_unused]] NonnullRefPtr<Media::VideoSurface> const& surface)
{
#if defined(AK_OS_MACOS)
    TRY(encoder.append_attachment(Attachment::from_mach_port(surface->create_mach_port(), Core::MachPort::MessageRight::MoveSend)));
    return {};
#elif defined(AK_OS_LINUX)
    TRY(encoder.encode(surface->id()));
    TRY(encoder.encode(surface->dma_buf()));
    return {};
#else
    return Error::from_string_literal("This platform has no video surfaces to encode");
#endif
}

template<>
ErrorOr<NonnullRefPtr<Media::VideoSurface>> decode([[maybe_unused]] Decoder& decoder)
{
#if defined(AK_OS_MACOS)
    auto attachment = TRY(decoder.attachments().try_dequeue());
    if (attachment.message_right() != Core::MachPort::MessageRight::MoveSend)
        return Error::from_string_literal("VideoSurface attachment is not a moved send right");
    return TRY(Media::VideoSurface::create_from_mach_port(attachment.release_mach_port()));
#elif defined(AK_OS_LINUX)
    auto id = TRY(decoder.decode<u64>());
    auto dma_buf = TRY(decoder.decode<Gfx::LinuxDmaBufHandle>());
    return TRY(Media::VideoSurface::create_from_dma_buf(id, move(dma_buf)));
#else
    return Error::from_string_literal("This platform has no video surfaces to decode");
#endif
}

}
