/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Platform.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Media {

class VideoSurfaceUse;

// A frame's pixels in memory that a GPU can sample without a copy. Hardware decoders allocate these and recycle a
// bounded set of them, so each carries the identity its recycling can be recognized by. Holding one keeps it alive
// and keeps that identity unambiguous, which is what lets a recycled surface be recognized as the one it was;
// begin_use() is what says its pixels are being read or written.
class MEDIA_API VideoSurface : public AtomicRefCounted<VideoSurface> {
public:
    ~VideoSurface();

    u32 id() const { return m_id; }

    [[nodiscard]] VideoSurfaceUse begin_use();
    bool is_in_use() const;

#ifdef AK_OS_MACOS
    static ErrorOr<NonnullRefPtr<VideoSurface>> create(Core::IOSurfaceHandle);
    static ErrorOr<NonnullRefPtr<VideoSurface>> create_from_mach_port(Core::MachPort const&);
    Core::MachPort create_mach_port() const { return m_io_surface.create_mach_port(); }
    Core::IOSurfaceHandle const& io_surface() const { return m_io_surface; }
#endif

private:
    friend class VideoSurfaceUse;

#ifdef AK_OS_MACOS
    VideoSurface(Core::IOSurfaceHandle io_surface, u32 id)
        : m_io_surface(move(io_surface))
        , m_id(id)
    {
    }

    Core::IOSurfaceHandle m_io_surface;
#endif

    u32 m_id { 0 };
};

// Says that a surface's pixels are being read or written for as long as it exists. The decoder that allocated the
// surface will not decode into it again while any of these are alive.
class MEDIA_API VideoSurfaceUse {
    AK_MAKE_NONCOPYABLE(VideoSurfaceUse);

public:
    VideoSurfaceUse() = default;
    VideoSurfaceUse(VideoSurfaceUse&&);
    VideoSurfaceUse& operator=(VideoSurfaceUse&&);
    ~VideoSurfaceUse();

private:
    friend class VideoSurface;

    explicit VideoSurfaceUse(VideoSurface&);
    void release();

    RefPtr<VideoSurface> m_surface;
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, RefPtr<Media::VideoSurface> const&);

template<>
MEDIA_API ErrorOr<RefPtr<Media::VideoSurface>> decode(Decoder&);

}
