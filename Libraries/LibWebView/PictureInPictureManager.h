/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/Size.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Owns the Picture-in-Picture windows the browser shows, each with the page whose video it shows. A page that opens a
// window takes it from any other page that had one. A window shows a page of its own, which the requesting page created
// in its process.
class WEBVIEW_API PictureInPictureManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static PictureInPictureManager& the();

    // Returns the window's handle, or an empty string when no window could be opened.
    String open_window(WebContentPage& requesting_page, CanonicalTraversable&, Gfx::IntSize video_size);
    void close_window(WebContentPage&);
    void video_size_did_change(WebContentPage&, Gfx::IntSize);

private:
    struct Window {
        NonnullRefPtr<WebContentPage> page;
        NonnullOwnPtr<PictureInPictureWindow> window;
    };

    Optional<size_t> index_of_window_for(WebContentPage const&) const;
    Optional<size_t> index_of(PictureInPictureWindow const&) const;
    void retire_window(size_t index);
    void window_did_close(PictureInPictureWindow&);
    void window_page_did_close(PictureInPictureWindow&);

    Vector<Window> m_windows;
    Vector<NonnullOwnPtr<PictureInPictureWindow>> m_retired_windows;
};

}
