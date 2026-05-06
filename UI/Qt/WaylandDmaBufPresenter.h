/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Size.h>
#include <LibIPC/File.h>

class QWidget;

namespace Ladybird {

struct DmaBufPresentationBuffer {
    u32 drm_format { 0 };
    u32 stride { 0 };
    u32 offset { 0 };
    IPC::File fd;
    u32 width { 0 };
    u32 height { 0 };
};

class WaylandDmaBufPresenter {
public:
    enum class PresentResult : u8 {
        Presented,
        Busy,
        Failed,
    };

    WaylandDmaBufPresenter(Function<void()> request_repaint, Function<void(u64 present_id)> on_presented);
    ~WaylandDmaBufPresenter();

    bool has_buffer(u64 image_id) const;
    PresentResult present_existing(QWidget& widget, u64 image_id, u64 present_id, Gfx::IntSize frame_size);
    PresentResult present(QWidget& widget, u64 image_id, u64 present_id, Gfx::IntSize frame_size, DmaBufPresentationBuffer buffer);
    void reset();

private:
    struct Impl;
    OwnPtr<Impl> m_impl;
};

}
