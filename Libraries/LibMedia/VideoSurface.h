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
#include <LibMedia/Export.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Media {

// A frame's pixels in memory that a GPU can sample without a copy. Hardware decoders allocate these and recycle a
// bounded set of them, so each carries the identity its recycling can be recognized by.
class MEDIA_API VideoSurface : public AtomicRefCounted<VideoSurface> {
public:
    ~VideoSurface();

    u32 id() const { return m_id; }

#ifdef AK_OS_MACOS
    static ErrorOr<NonnullRefPtr<VideoSurface>> create(Core::IOSurfaceHandle);
    Core::MachPort create_mach_port() const { return m_io_surface.create_mach_port(); }
#endif

private:
#ifdef AK_OS_MACOS
    VideoSurface(Core::IOSurfaceHandle io_surface, u32 id)
        : m_io_surface(move(io_surface))
        , m_id(id)
    {
        m_io_surface.increment_use_count();
    }

    Core::IOSurfaceHandle m_io_surface;
#endif

    u32 m_id { 0 };
};

}
