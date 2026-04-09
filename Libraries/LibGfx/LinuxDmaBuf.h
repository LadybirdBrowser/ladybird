/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibIPC/File.h>

namespace Gfx {

struct LinuxDmaBufPlaneLayout {
    u32 stride { 0 };
    u32 offset { 0 };
};

struct LinuxDmaBufBackingStore {
    u32 drm_format { 0 };
    u64 modifier { 0 };
    LinuxDmaBufPlaneLayout plane;
    IPC::File fd;
    IntSize size;
};

}
