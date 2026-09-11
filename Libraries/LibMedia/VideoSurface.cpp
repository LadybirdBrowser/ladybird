/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/VideoSurface.h>

namespace Media {

#ifdef AK_OS_MACOS

VideoSurface::~VideoSurface() = default;

bool VideoSurface::is_in_use() const
{
    return m_io_surface.is_in_use();
}

VideoSurfaceUse::VideoSurfaceUse(VideoSurface& surface)
    : m_surface(surface)
{
    m_surface->m_io_surface.increment_use_count();
}

void VideoSurfaceUse::release()
{
    if (m_surface == nullptr)
        return;
    m_surface->m_io_surface.decrement_use_count();
    m_surface = nullptr;
}

ErrorOr<NonnullRefPtr<VideoSurface>> VideoSurface::create(Core::IOSurfaceHandle io_surface)
{
    auto id = io_surface.id();
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoSurface(move(io_surface), id));
}

ErrorOr<NonnullRefPtr<VideoSurface>> VideoSurface::create_from_mach_port(Core::MachPort const& port)
{
    return create(TRY(Core::IOSurfaceHandle::from_mach_port(port)));
}

#else

VideoSurface::~VideoSurface() = default;

bool VideoSurface::is_in_use() const
{
    return false;
}

VideoSurfaceUse::VideoSurfaceUse(VideoSurface& surface)
    : m_surface(surface)
{
}

void VideoSurfaceUse::release()
{
    m_surface = nullptr;
}

#endif

VideoSurfaceUse VideoSurface::begin_use()
{
    return VideoSurfaceUse { *this };
}

VideoSurfaceUse::VideoSurfaceUse(VideoSurfaceUse&& other)
    : m_surface(move(other.m_surface))
{
}

VideoSurfaceUse& VideoSurfaceUse::operator=(VideoSurfaceUse&& other)
{
    if (this != &other) {
        release();
        m_surface = move(other.m_surface);
    }
    return *this;
}

VideoSurfaceUse::~VideoSurfaceUse()
{
    release();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, RefPtr<Media::VideoSurface> const& surface)
{
    TRY(encoder.encode(surface != nullptr));
    if (surface == nullptr)
        return {};

#ifdef AK_OS_MACOS
    TRY(encoder.append_attachment(Attachment::from_mach_port(surface->create_mach_port(), Core::MachPort::MessageRight::MoveSend)));
    return {};
#else
    return Error::from_string_literal("This platform has no video surfaces to encode");
#endif
}

template<>
ErrorOr<RefPtr<Media::VideoSurface>> decode(Decoder& decoder)
{
    if (!TRY(decoder.decode<bool>()))
        return RefPtr<Media::VideoSurface> {};

#ifdef AK_OS_MACOS
    auto attachment = TRY(decoder.attachments().try_dequeue());
    if (attachment.message_right() != Core::MachPort::MessageRight::MoveSend)
        return Error::from_string_literal("VideoSurface attachment is not a moved send right");
    return TRY(Media::VideoSurface::create_from_mach_port(attachment.release_mach_port()));
#else
    return Error::from_string_literal("This platform has no video surfaces to decode");
#endif
}

}
