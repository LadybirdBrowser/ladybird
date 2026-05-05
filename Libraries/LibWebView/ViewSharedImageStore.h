/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImagePayload.h>
#include <LibPaintServer/Presentation.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Export.h>

namespace WebView {

class ViewImplementation;

class WEBVIEW_API ViewSharedImageStore {
public:
    struct PresentableFrame {
        enum class Source {
            PresentationSurface,
        };

        Source source;
        Gfx::Bitmap const* bitmap { nullptr };
        Gfx::IntSize bitmap_size;

        Optional<void*> platform_surface_handle;
        Gfx::IntSize surface_size;
        Optional<u64> present_id;
        Optional<u64> image_id;
    };

    explicit ViewSharedImageStore(ViewImplementation&);
    ~ViewSharedImageStore();

    void reset();

    Optional<PresentableFrame> presentable_frame() const;
    Optional<LinuxDmaBufPresentationBuffer> clone_linux_dmabuf_presentation_buffer(u64 image_id) const;
    RefPtr<Gfx::Bitmap const> bitmap_for_presentation_image(u64 image_id) const;

    void did_receive_presentation_frame(u64 present_id, u64 image_id, Gfx::IntSize frame_size);
    void did_submit_presentation_frame(u64 present_id);
    void did_present_frame(u64 present_id);

    void configure_presentation_surface(Gfx::IntSize size);
    void ensure_presentation_buffers(Gfx::IntSize size);

private:
    ViewImplementation& m_view;

    Optional<u64> m_pending_present_id;
    bool m_pending_present_was_submitted { false };

    Optional<u64> m_presentation_current_image_id;
    Gfx::IntSize m_presentation_current_frame_size;
};

}
