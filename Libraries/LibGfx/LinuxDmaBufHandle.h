/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

namespace Gfx {

struct LinuxDmaBufPlane {
    u32 object_index { 0 };
    u32 offset { 0 };
    u32 pitch { 0 };
};

// An image in one or more dma-buf objects. The format and modifier say how many planes it has and how each lays out
// its pixels, and each plane lies in one of the objects.
struct LinuxDmaBufHandle {
    static constexpr size_t MAXIMUM_PLANE_COUNT = 4;

    IntSize size;
    u32 drm_format { 0 };
    u64 modifier { 0 };
    Vector<IPC::File, MAXIMUM_PLANE_COUNT> objects;
    Vector<LinuxDmaBufPlane, MAXIMUM_PLANE_COUNT> planes;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::LinuxDmaBufHandle const&);

template<>
ErrorOr<Gfx::LinuxDmaBufHandle> decode(Decoder&);

}
