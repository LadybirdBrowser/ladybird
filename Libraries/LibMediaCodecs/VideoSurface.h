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
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Export.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif
#ifdef AK_OS_LINUX
#    include <LibGfx/LinuxDmaBufHandle.h>
#endif

namespace Media {

// What the process that decoded into a surface holds to keep those pixels from being decoded over, for as long as
// the frame slot they were handed out in is held anywhere.
class VideoSurfaceContents : public AtomicRefCounted<VideoSurfaceContents> {
public:
    virtual ~VideoSurfaceContents() = default;
};

// A frame's pixels in memory that a GPU can sample without a copy. Hardware decoders allocate these and recycle a
// bounded set of them, so each carries the identity its recycling is recognized by.
class MEDIACODECS_API VideoSurface : public AtomicRefCounted<VideoSurface> {
public:
    ~VideoSurface();

    // Tells the surface apart from every other its sender has handed out, including from an earlier decoder process.
    u64 id() const { return m_id; }
    Gfx::IntSize size() const;

    // Reads the pixels back into memory, which works only where the platform lets this process map the surface.
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> read_back_bitmap(CodingIndependentCodePoints const&) const;

#ifdef AK_OS_MACOS
    static ErrorOr<NonnullRefPtr<VideoSurface>> create(Core::IOSurfaceHandle);
    static ErrorOr<NonnullRefPtr<VideoSurface>> create_from_mach_port(Core::MachPort const&);
    Core::MachPort create_mach_port() const { return m_io_surface.create_mach_port(); }
    Core::IOSurfaceHandle const& io_surface() const { return m_io_surface; }
#endif

#ifdef AK_OS_LINUX
    // The decoder assigns the ID, since the memory behind an image can be exported as any number of file
    // descriptors. The size is the image's, which can exceed the frames decoded into it.
    static ErrorOr<NonnullRefPtr<VideoSurface>> create_from_dma_buf(u64 id, Gfx::LinuxDmaBufHandle);

    Gfx::LinuxDmaBufHandle const& dma_buf() const { return m_dma_buf; }
#endif

private:
#ifdef AK_OS_MACOS
    VideoSurface(Core::IOSurfaceHandle io_surface, u64 id)
        : m_io_surface(move(io_surface))
        , m_id(id)
    {
    }

    Core::IOSurfaceHandle m_io_surface;
#endif

#ifdef AK_OS_LINUX
    VideoSurface(u64 id, Gfx::LinuxDmaBufHandle dma_buf)
        : m_dma_buf(move(dma_buf))
        , m_id(id)
    {
    }

    Gfx::LinuxDmaBufHandle m_dma_buf;
#endif

    u64 m_id { 0 };
};

}

namespace IPC {

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, NonnullRefPtr<Media::VideoSurface> const&);

template<>
MEDIACODECS_API ErrorOr<NonnullRefPtr<Media::VideoSurface>> decode(Decoder&);

}
