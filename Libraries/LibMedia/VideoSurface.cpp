/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/VideoSurface.h>

#ifdef AK_OS_MACOS

namespace Media {

VideoSurface::~VideoSurface()
{
    m_io_surface.decrement_use_count();
}

ErrorOr<NonnullRefPtr<VideoSurface>> VideoSurface::create(Core::IOSurfaceHandle io_surface)
{
    auto id = io_surface.id();
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoSurface(move(io_surface), id));
}

}

#else

VideoSurface::~VideoSurface() = default;

#endif
